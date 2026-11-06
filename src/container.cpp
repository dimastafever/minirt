#include "container.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common.hpp"

namespace rt {
namespace {

[[noreturn]] void init_fail(const std::string& what) {
    dprintf(2, "container: %s: %s\n", what.c_str(), strerror(errno));
    _exit(125);
}

void make_dir(const std::string& p) { mkdir(p.c_str(), 0755); }  // EEXIST is fine
void sym(const char* target, const std::string& path) {
    if (symlink(target, path.c_str()) < 0) {}  // best effort
}

void bind_dev(const std::string& dev, const char* name) {
    std::string dst = dev + "/" + name, src = std::string("/dev/") + name;
    int fd = open(dst.c_str(), O_CREAT | O_WRONLY, 0666);
    if (fd < 0) init_fail("create " + dst);
    close(fd);
    if (mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) < 0) init_fail("bind " + src);
}

void setup_rootfs(const ContainerConfig& c) {
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) init_fail("make / private");

    char buf[PATH_MAX];
    if (!realpath(c.rootfs.c_str(), buf)) init_fail("realpath " + c.rootfs);
    std::string root = buf;

    if (c.overlay) {
        std::string up = c.overlay_dir + "/upper", wk = c.overlay_dir + "/work",
                    merged = c.overlay_dir + "/merged";
        mkdir_p(up);
        mkdir_p(wk);
        mkdir_p(merged);
        std::string opts = "lowerdir=" + root + ",upperdir=" + up + ",workdir=" + wk + ",userxattr";
        if (mount("overlay", merged.c_str(), "overlay", 0, opts.c_str()) < 0)
            init_fail("mount overlayfs (needs Linux >= 5.11)");
        root = merged;
    }

    if (mount(root.c_str(), root.c_str(), nullptr, MS_BIND | MS_REC, nullptr) < 0) init_fail("bind rootfs");

    make_dir(root + "/proc");
    if (mount("proc", (root + "/proc").c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0)
        init_fail("mount /proc");

    std::string dev = root + "/dev";
    make_dir(dev);
    if (mount("tmpfs", dev.c_str(), "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=64k") < 0)
        init_fail("mount /dev");
    for (const char* n : {"null", "zero", "full", "random", "urandom", "tty"}) bind_dev(dev, n);
    make_dir(dev + "/pts");
    mount("devpts", (dev + "/pts").c_str(), "devpts", MS_NOSUID | MS_NOEXEC,
          "newinstance,ptmxmode=0666,mode=0620");  // best effort
    make_dir(dev + "/shm");
    mount("tmpfs", (dev + "/shm").c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=64m");
    sym("pts/ptmx", dev + "/ptmx");
    sym("/proc/self/fd", dev + "/fd");
    sym("/proc/self/fd/0", dev + "/stdin");
    sym("/proc/self/fd/1", dev + "/stdout");
    sym("/proc/self/fd/2", dev + "/stderr");
    std::string old = root + "/.oldroot";
    make_dir(old);
    if (syscall(SYS_pivot_root, root.c_str(), old.c_str()) < 0) init_fail("pivot_root");
    if (chdir("/") < 0) init_fail("chdir /");
    if (umount2("/.oldroot", MNT_DETACH) < 0) init_fail("umount old root");
    rmdir("/.oldroot");
}

void bring_up_loopback() {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) init_fail("socket");
    ifreq ifr{};
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) init_fail("ioctl SIOCGIFFLAGS");
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) init_fail("bring up lo");
    close(s);
}

[[noreturn]] void exec_command(const ContainerConfig& c) {
    const char* term = getenv("TERM");
    std::string term_s = term ? term : "xterm";
    clearenv();
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", term_s.c_str(), 1);
    setenv("HOSTNAME", c.hostname.c_str(), 1);
    for (const auto& e : c.env) putenv(strdup(e.c_str()));

    std::vector<char*> argv;
    for (const auto& a : c.cmd) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    dprintf(2, "container: cannot execute %s: %s\n", argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

[[noreturn]] void init_main(const ContainerConfig& c) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // never outlive the shim
    if (c.ctty) {                      // PTY mode: become session leader, own the terminal
        setsid();
        ioctl(0, TIOCSCTTY, 1);
    }
    if (sethostname(c.hostname.c_str(), c.hostname.size()) < 0) init_fail("sethostname");
    setup_rootfs(c);
    if (c.net_from <= 0) bring_up_loopback();  // joined network already has its own lo
    exec_command(c);
}

pid_t g_init_pid = 0;
void forward_signal(int sig) {
    if (g_init_pid > 0) kill(g_init_pid, sig);
}

void close_other_fds(int keep) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++)
            if (fd != keep) close(fd);
        return;
    }
    int dfd = dirfd(d);
    std::vector<int> fds;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        int fd = atoi(e->d_name);
        if (fd > 2 && fd != dfd && fd != keep) fds.push_back(fd);
    }
    closedir(d);
    for (int fd : fds) close(fd);
}

bool write_file(const char* path, const std::string& data) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    bool ok = write(fd, data.data(), data.size()) == (ssize_t)data.size();
    close(fd);
    return ok;
}

[[noreturn]] void run_shim(const ContainerConfig& c, int in, int out, int err, int status_fd) {
    for (int s = 1; s < NSIG; s++) signal(s, SIG_DFL);  // forget the daemon's handlers
    dup2(in, 0);
    dup2(out, 1);
    dup2(err, 2);
    close_other_fds(status_fd);  // do not leak daemon sockets into the container

    auto fail = [&](const std::string& what) {
        std::string m = "ERR " + what + ": " + strerror(errno);
        write_all(status_fd, m.data(), m.size());
        _exit(1);
    };

    if (c.net_from > 0) {
        std::string base = "/proc/" + std::to_string(c.net_from) + "/ns/";
        int ufd = open((base + "user").c_str(), O_RDONLY | O_CLOEXEC);
        int nfd = open((base + "net").c_str(), O_RDONLY | O_CLOEXEC);
        if (ufd < 0 || nfd < 0) fail("cannot open namespaces of pid " + std::to_string(c.net_from));
        if (setns(ufd, CLONE_NEWUSER) < 0) fail("setns(user)");
        if (setns(nfd, CLONE_NEWNET) < 0) fail("setns(net)");
        close(ufd);
        close(nfd);
        if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC) < 0) fail("unshare");
    } else {
        uid_t uid = getuid();
        gid_t gid = getgid();
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC |
                    CLONE_NEWNET) < 0)
            fail("unshare (are unprivileged user namespaces enabled?)");
        if (!write_file("/proc/self/setgroups", "deny") ||
            !write_file("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1\n") ||
            !write_file("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1\n"))
            fail("write uid/gid map");
    }
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    pid_t init = fork();  // first child after CLONE_NEWPID => PID 1
    if (init < 0) fail("fork");
    if (init == 0) init_main(c);

    std::string msg = "PID " + std::to_string(init);
    write_all(status_fd, msg.data(), msg.size());
    close(status_fd);

    g_init_pid = init;
    struct sigaction sa{};
    sa.sa_handler = forward_signal;
    sa.sa_flags = SA_RESTART;
    for (int s : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaction(s, &sa, nullptr);

    int st = 0;
    pid_t r;
    do {
        r = waitpid(init, &st, 0);
    } while (r < 0 && errno == EINTR);
    if (WIFEXITED(st)) _exit(WEXITSTATUS(st));
    if (WIFSIGNALED(st)) _exit(128 + WTERMSIG(st));
    _exit(1);
}

}  // namespace

bool spawn_container(const ContainerConfig& cfg, int in, int out, int err, Spawned& sp,
                     std::string& error) {
    struct stat st;
    if (cfg.cmd.empty()) {
        error = "no command given";
        return false;
    }
    if (stat(cfg.rootfs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        error = "rootfs is not a directory: " + cfg.rootfs;
        return false;
    }
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) {
        error = std::string("pipe: ") + strerror(errno);
        return false;
    }
    pid_t shim = fork();
    if (shim < 0) {
        error = std::string("fork: ") + strerror(errno);
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (shim == 0) run_shim(cfg, in, out, err, pfd[1]);

    close(pfd[1]);
    char buf[512];
    ssize_t n;
    do {
        n = read(pfd[0], buf, sizeof buf - 1);
    } while (n < 0 && errno == EINTR);
    close(pfd[0]);
    if (n > 4 && strncmp(buf, "PID ", 4) == 0) {
        buf[n] = 0;
        sp.shim = shim;
        sp.init = atoi(buf + 4);
        return true;
    }
    error = n > 0 ? std::string(buf, (size_t)n) : "container setup failed";
    if (error.compare(0, 4, "ERR ") == 0) error.erase(0, 4);
    int s;
    waitpid(shim, &s, 0);
    return false;
}

int exit_code_from_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

}  // namespace rt
