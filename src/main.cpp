#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "common.hpp"
#include "container.hpp"
#include "daemon.hpp"

using namespace rt;

namespace {

const char* USAGE = R"(usage: runtime <command> [options]

  run [opts] ROOTFS [CMD...]   run a container
        -t, --tty              allocate a PTY (interactive shell, Ctrl-C, resize...)
        -d, --detach           run in the background under the daemon (prints the id)
            --name NAME        container name (with -d)
            --hostname NAME    hostname inside the container
            --overlay          copy-on-write rootfs (overlayfs); ROOTFS stays pristine
            --net-from REF     share the network of another container
                               (REF = id, name or host pid with -d; pid without -d)
        -e KEY=VAL             set an environment variable (repeatable)
  daemon [-d]                  start the management daemon (-d = in the background)
  list                         list containers known to the daemon
  logs [-f] REF                print container output (-f = keep following)
  attach REF                   attach to the container terminal (detach: Ctrl-P Ctrl-Q)
  kill [-s SIG] [-t SEC] [--force] REF
                               stop a container: SIGTERM, then SIGKILL after SEC (default 5)
                               REF = id (or prefix), name, or host pid
  rm REF                       delete an exited container and its logs

environment: MINIRT_HOME = state directory (default ~/.minirt)
)";

volatile sig_atomic_t g_winch = 0;
void on_winch(int) { g_winch = 1; }

struct RawTerminal {
    int fd;
    bool active = false;
    termios saved{};
    explicit RawTerminal(int f) : fd(f) {
        if (isatty(fd) && tcgetattr(fd, &saved) == 0) {
            termios raw = saved;
            cfmakeraw(&raw);
            tcsetattr(fd, TCSAFLUSH, &raw);
            active = true;
        }
    }
    ~RawTerminal() {
        if (active) tcsetattr(fd, TCSAFLUSH, &saved);
    }
};

void install_winch() {
    struct sigaction sa{};
    sa.sa_handler = on_winch;  // no SA_RESTART: poll() must wake up
    sigaction(SIGWINCH, &sa, nullptr);
}

int connect_daemon() {
    int fd = connect_unix(socket_path());
    if (fd < 0) die("cannot connect to the daemon at %s (start it with: runtime daemon -d)", socket_path().c_str());
    return fd;
}

int request(const std::string& line, std::string& payload) {
    int fd = connect_daemon();
    if (!write_all(fd, (line + "\n").data(), line.size() + 1)) die("write to daemon failed");
    std::string st;
    if (!read_line(fd, st)) die("daemon closed the connection");
    if (st.compare(0, 3, "ERR") == 0) {
        fprintf(stderr, "error: %s\n", st.size() > 4 ? st.c_str() + 4 : "request failed");
        close(fd);
        return -1;
    }
    payload = st.size() > 3 ? st.substr(3) : "";
    return fd;
}

int simple_request(const std::string& line) {
    std::string payload;
    int fd = request(line, payload);
    if (fd < 0) return 1;
    if (!payload.empty()) write_all(1, (payload + "\n").data(), payload.size() + 1);
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0) write_all(1, buf, (size_t)n);
    close(fd);
    return 0;
}

int parse_signal(const std::string& s) {
    std::string n = s.compare(0, 3, "SIG") == 0 ? s.substr(3) : s;
    if (!n.empty() && isdigit((unsigned char)n[0])) return atoi(n.c_str());
    static const struct { const char* name; int num; } table[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"USR1", SIGUSR1},
        {"USR2", SIGUSR2}, {"TERM", SIGTERM}, {"CONT", SIGCONT}, {"STOP", SIGSTOP}};
    for (auto& e : table)
        if (n == e.name) return e.num;
    die("unknown signal: %s", s.c_str());
}

struct RunOpts {
    bool tty = false, detach = false, overlay = false;
    std::string name, hostname, net, rootfs;
    std::vector<std::string> env, cmd;
};

RunOpts parse_run(int argc, char** argv) {
    RunOpts o;
    int i = 2;
    auto need = [&](const char* opt) -> std::string {
        if (i + 1 >= argc) die("option %s needs a value", opt);
        return argv[++i];
    };
    for (; i < argc && argv[i][0] == '-'; i++) {
        std::string a = argv[i];
        if (a == "-t" || a == "--tty") o.tty = true;
        else if (a == "-d" || a == "--detach") o.detach = true;
        else if (a == "--overlay") o.overlay = true;
        else if (a == "--name") o.name = need("--name");
        else if (a == "--hostname") o.hostname = need("--hostname");
        else if (a == "--net-from") o.net = need("--net-from");
        else if (a == "-e") o.env.push_back(need("-e"));
        else if (a == "--") { i++; break; }
        else die("unknown option for run: %s", a.c_str());
    }
    if (i >= argc) die("run: ROOTFS is required\n\n%s", USAGE);
    char buf[PATH_MAX];
    if (!realpath(argv[i], buf)) die("rootfs not found: %s", argv[i]);
    o.rootfs = buf;
    for (i++; i < argc; i++) o.cmd.push_back(argv[i]);
    if (o.cmd.empty()) o.cmd.push_back("/bin/sh");
    return o;
}

int run_with_pty(ContainerConfig cfg) {
    int master, slave;
    if (!open_pty(master, slave)) die("cannot allocate a PTY");
    winsize ws{};
    if (ioctl(0, TIOCGWINSZ, &ws) == 0) ioctl(master, TIOCSWINSZ, &ws);
    cfg.ctty = true;
    Spawned sp;
    std::string err;
    if (!spawn_container(cfg, slave, slave, slave, sp, err)) die("%s", err.c_str());
    close(slave);

    {
        RawTerminal raw(0);
        install_winch();
        bool stdin_open = true;
        char buf[4096];
        for (;;) {
            if (g_winch) {
                g_winch = 0;
                if (ioctl(0, TIOCGWINSZ, &ws) == 0) ioctl(master, TIOCSWINSZ, &ws);
            }
            pollfd p[2] = {{0, (short)(stdin_open ? POLLIN : 0), 0}, {master, POLLIN, 0}};
            if (poll(p, 2, -1) < 0) continue;  // EINTR (SIGWINCH)
            if (p[0].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(0, buf, sizeof buf);
                if (n > 0) write_all(master, buf, (size_t)n);
                else stdin_open = false;
            }
            if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = read(master, buf, sizeof buf);
                if (n <= 0) break;  // EIO: container side closed
                write_all(1, buf, (size_t)n);
            }
        }
    }
    close(master);
    int st = 0;
    waitpid(sp.shim, &st, 0);
    return exit_code_from_status(st);
}

int cmd_run(int argc, char** argv) {
    RunOpts o = parse_run(argc, argv);

    if (o.detach) {  // hand the container over to the daemon
        std::string req = "RUN rootfs=" + urlenc(o.rootfs);
        if (!o.name.empty()) req += " name=" + urlenc(o.name);
        if (!o.hostname.empty()) req += " hostname=" + urlenc(o.hostname);
        if (!o.net.empty()) req += " net=" + urlenc(o.net);
        if (o.overlay) req += " overlay=1";
        for (auto& e : o.env) req += " env=" + urlenc(e);
        for (auto& c : o.cmd) req += " cmd=" + urlenc(c);
        return simple_request(req);
    }

    ContainerConfig cfg;
    cfg.rootfs = o.rootfs;
    cfg.cmd = o.cmd;
    cfg.env = o.env;
    if (!o.hostname.empty()) cfg.hostname = o.hostname;
    if (!o.net.empty()) {
        cfg.net_from = atoi(o.net.c_str());
        if (cfg.net_from <= 0) die("without -d, --net-from needs a host pid");
    }
    std::string tmp;
    if (o.overlay) {
        char tmpl[] = "/tmp/minirt-overlay-XXXXXX";
        if (!mkdtemp(tmpl)) die("mkdtemp: %s", strerror(errno));
        tmp = tmpl;
        cfg.overlay = true;
        cfg.overlay_dir = tmp;
    }

    int code;
    if (o.tty) {
        code = run_with_pty(cfg);
    } else {
        Spawned sp;
        std::string err;
        if (!spawn_container(cfg, 0, 1, 2, sp, err)) die("%s", err.c_str());
        int st = 0;
        while (waitpid(sp.shim, &st, 0) < 0 && errno == EINTR) {}
        code = exit_code_from_status(st);
    }
    if (!tmp.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }
    return code;
}

int cmd_attach(const std::string& ref) {
    std::string payload;
    int fd = request("ATTACH ref=" + urlenc(ref), payload);
    if (fd < 0) return 1;

    RawTerminal raw(0);
    install_winch();
    bool stdin_open = true, detached = false, pending_ctrl_p = false;

    auto send_size = [&]() {
        winsize ws{};
        if (ioctl(0, TIOCGWINSZ, &ws) != 0) return;
        unsigned char b[4] = {(unsigned char)(ws.ws_row >> 8), (unsigned char)ws.ws_row,
                              (unsigned char)(ws.ws_col >> 8), (unsigned char)ws.ws_col};
        send_frame(fd, FRAME_RESIZE, b, 4);
    };
    send_size();

    char buf[4096];
    for (;;) {
        if (g_winch) {
            g_winch = 0;
            send_size();
        }
        pollfd p[2] = {{0, (short)(stdin_open ? POLLIN : 0), 0}, {fd, POLLIN, 0}};
        if (poll(p, 2, -1) < 0) continue;
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                stdin_open = false;  // e.g. `echo ls | runtime attach x`: keep printing output
            } else {
                std::string out;
                for (ssize_t i = 0; i < n; i++) {
                    unsigned char b = (unsigned char)buf[i];
                    if (pending_ctrl_p) {
                        pending_ctrl_p = false;
                        if (b == 0x11) { detached = true; break; }
                        out += (char)0x10;
                    }
                    if (b == 0x10) { pending_ctrl_p = true; continue; }
                    out += (char)b;
                }
                if (!out.empty()) send_frame(fd, FRAME_DATA, out.data(), out.size());
                if (detached) break;
            }
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;  // daemon closed: the container exited
            write_all(1, buf, (size_t)n);
        }
    }
    close(fd);
    fprintf(stderr, "\r\n[%s]\r\n", detached ? "detached" : "container exited");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        fputs(USAGE, stderr);
        return 2;
    }
    std::string cmd = argv[1];

    if (cmd == "run") return cmd_run(argc, argv);

    if (cmd == "daemon") {
        bool detach = argc > 2 && (!strcmp(argv[2], "-d") || !strcmp(argv[2], "--detach"));
        return run_daemon(detach);
    }
    if (cmd == "list" || cmd == "ps") return simple_request("LIST");

    if (cmd == "logs") {
        bool follow = false;
        std::string ref;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--follow")) follow = true;
            else ref = argv[i];
        }
        if (ref.empty()) die("logs: REF is required");
        return simple_request("LOGS ref=" + urlenc(ref) + (follow ? " follow=1" : ""));
    }

    if (cmd == "attach") {
        if (argc < 3) die("attach: REF is required");
        return cmd_attach(argv[2]);
    }

    if (cmd == "kill") {
        std::string ref, sig, timeout;
        bool force = false;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "-s" && i + 1 < argc) sig = std::to_string(parse_signal(argv[++i]));
            else if (a == "-t" && i + 1 < argc) timeout = argv[++i];
            else if (a == "--force" || a == "-9") force = true;
            else ref = a;
        }
        if (ref.empty()) die("kill: REF is required");
        std::string req = "KILL ref=" + urlenc(ref);
        if (force) req += " force=1";
        if (!sig.empty()) req += " sig=" + sig;
        if (!timeout.empty()) req += " timeout=" + timeout;
        return simple_request(req);
    }

    if (cmd == "rm") {
        if (argc < 3) die("rm: REF is required");
        return simple_request("RM ref=" + urlenc(argv[2]));
    }

    fputs(USAGE, stderr);
    return 2;
}
