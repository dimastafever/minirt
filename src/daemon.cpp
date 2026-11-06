#include "daemon.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>

#include "common.hpp"
#include "container.hpp"

namespace rt {
namespace {

struct Container {
    std::string id, name, rootfs, dir;
    std::vector<std::string> cmd;
    pid_t shim = 0, init = 0;
    int master = -1;  // PTY master: container output comes out, input goes in
    int logfd = -1;
    bool running = true;
    int exit_code = 0;
    time_t kill_deadline = 0;  // graceful stop: SIGKILL when this time is reached
    std::string pending_in;    // bytes waiting to be written to the PTY
};

struct Client {
    enum Mode { REQUEST, LOGS, ATTACH };
    int fd = -1;
    Mode mode = REQUEST;
    Container* c = nullptr;  // for LOGS(follow)/ATTACH
    std::string in, out;
    bool close_after_flush = false;
};

std::vector<std::unique_ptr<Container>> g_containers;
std::map<int, Client> g_clients;
int g_wake[2];  // self-pipe
volatile sig_atomic_t g_stop = 0;

void on_signal(int sig) {
    if (sig != SIGCHLD) g_stop = 1;
    int e = errno;
    if (write(g_wake[1], "x", 1) < 0) {}
    errno = e;
}
std::string new_id() {
    static std::random_device rd;
    for (;;) {
        char buf[16];
        snprintf(buf, sizeof buf, "%08x", rd());
        bool used = false;
        for (auto& c : g_containers) used |= (c->id == buf);
        if (!used) return buf;
    }
}

Container* find(const std::string& ref, std::string& err) {
    for (auto& up : g_containers) {  // exact id, exact name, or host pid of PID 1
        Container* c = up.get();
        if (c->id == ref || (!c->name.empty() && c->name == ref) ||
            (c->running && std::to_string(c->init) == ref))
            return c;
    }
    Container* match = nullptr;
    int n = 0;
    for (auto& up : g_containers)
        if (!ref.empty() && up->id.compare(0, ref.size(), ref) == 0) match = up.get(), n++;
    if (n == 1) return match;
    err = (n == 0 ? "no such container: " : "ambiguous container: ") + ref;
    return nullptr;
}

void queue(Client& cl, const std::string& data) {
    cl.out += data;
    if (cl.out.size() > (8u << 20)) {  // client too slow: drop it instead of eating memory
        cl.out.clear();
        cl.close_after_flush = true;
    }
}

void reply_ok(Client& cl, const std::string& payload = "") {
    queue(cl, payload.empty() ? "OK\n" : "OK " + payload + "\n");
}
void reply_err(Client& cl, const std::string& msg) {
    queue(cl, "ERR " + msg + "\n");
    cl.close_after_flush = true;
}

// Send container output to everyone watching it (attach + logs -f).
void broadcast(Container* c, const char* data, size_t n) {
    for (auto& [fd, cl] : g_clients)
        if (cl.c == c && (cl.mode == Client::ATTACH || cl.mode == Client::LOGS)) queue(cl, std::string(data, n));
}

bool drain_master(Container* c) {
    char buf[4096];
    while (c->master >= 0) {
        ssize_t n = read(c->master, buf, sizeof buf);
        if (n > 0) {
            if (c->logfd >= 0) write_all(c->logfd, buf, (size_t)n);
            broadcast(c, buf, (size_t)n);
            continue;
        }
        return n == 0 || errno == EIO;  // otherwise EAGAIN: nothing more right now
    }
    return false;
}

void flush_pty(Container* c) {
    while (c->master >= 0 && !c->pending_in.empty()) {
        ssize_t n = write(c->master, c->pending_in.data(), c->pending_in.size());
        if (n <= 0) break;
        c->pending_in.erase(0, (size_t)n);
    }
}

void container_finished(Container* c, int status) {
    drain_master(c);
    if (c->master >= 0) close(c->master), c->master = -1;
    if (c->logfd >= 0) close(c->logfd), c->logfd = -1;
    c->running = false;
    c->exit_code = exit_code_from_status(status);
    c->kill_deadline = 0;
    fprintf(stderr, "container %s exited with code %d\n", c->id.c_str(), c->exit_code);
    for (auto& [fd, cl] : g_clients)
        if (cl.c == c) {
            cl.c = nullptr;
            cl.close_after_flush = true;  // EOF tells attach/logs -f that it is over
        }
}

struct Request {
    std::string verb;
    std::map<std::string, std::vector<std::string>> kv;
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv.find(k);
        return it == kv.end() || it->second.empty() ? def : it->second[0];
    }
    std::vector<std::string> all(const std::string& k) const {
        auto it = kv.find(k);
        return it == kv.end() ? std::vector<std::string>{} : it->second;
    }
};

Request parse_request(const std::string& line) {
    Request rq;
    auto toks = split_ws(line);
    if (toks.empty()) return rq;
    rq.verb = toks[0];
    for (size_t i = 1; i < toks.size(); i++) {
        size_t eq = toks[i].find('=');
        if (eq == std::string::npos) continue;
        rq.kv[toks[i].substr(0, eq)].push_back(urldec(toks[i].substr(eq + 1)));
    }
    return rq;
}

void do_run(Client& cl, const Request& rq) {
    std::string name = rq.get("name"), rootfs = rq.get("rootfs");
    if (rootfs.empty()) return reply_err(cl, "rootfs is required");
    for (char ch : name)
        if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-' && ch != '.')
            return reply_err(cl, "invalid name (use letters, digits, _ - .)");
    for (auto& c : g_containers)
        if (!name.empty() && c->name == name) return reply_err(cl, "name already in use: " + name);

    ContainerConfig cfg;
    cfg.rootfs = rootfs;
    cfg.cmd = rq.all("cmd");
    cfg.env = rq.all("env");
    cfg.hostname = rq.get("hostname", name.empty() ? "container" : name);
    cfg.overlay = rq.get("overlay") == "1";
    cfg.ctty = true;  // daemon-managed containers always get a PTY

    std::string net = rq.get("net");
    if (!net.empty()) {
        std::string err;
        Container* other = find(net, err);
        if (!other) return reply_err(cl, err);
        if (!other->running) return reply_err(cl, "container is not running: " + net);
        cfg.net_from = other->init;
    }

    auto c = std::make_unique<Container>();
    c->id = new_id();
    c->name = name;
    c->rootfs = rootfs;
    c->cmd = cfg.cmd;
    c->dir = home_dir() + "/containers/" + c->id;
    mkdir_p(c->dir);
    cfg.overlay_dir = c->dir + "/overlay";
    c->logfd = open((c->dir + "/log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    int master, slave;
    if (!open_pty(master, slave)) return reply_err(cl, "cannot allocate a PTY");
    winsize ws{};
    ws.ws_row = 24;
    ws.ws_col = 80;
    ioctl(master, TIOCSWINSZ, &ws);

    Spawned sp;
    std::string error;
    bool ok = spawn_container(cfg, slave, slave, slave, sp, error);
    close(slave);  // the container holds the only slave copies now => EIO when it exits
    if (!ok) {
        close(master);
        if (c->logfd >= 0) close(c->logfd);
        std::error_code ec;
        std::filesystem::remove_all(c->dir, ec);
        return reply_err(cl, error);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    c->master = master;
    c->shim = sp.shim;
    c->init = sp.init;
    fprintf(stderr, "started container %s (pid %d)\n", c->id.c_str(), c->init);
    reply_ok(cl, c->id);
    g_containers.push_back(std::move(c));
}

void do_list(Client& cl) {
    char line[1024];
    snprintf(line, sizeof line, "%-10s %-14s %-8s %-12s %s\n", "ID", "NAME", "PID", "STATUS", "COMMAND");
    std::string out = "OK\n";
    out += line;
    for (auto& c : g_containers) {
        std::string status = c->running ? "running" : "exited(" + std::to_string(c->exit_code) + ")";
        std::string cmd;
        for (auto& a : c->cmd) cmd += (cmd.empty() ? "" : " ") + a;
        snprintf(line, sizeof line, "%-10s %-14s %-8s %-12s %s\n", c->id.c_str(),
                 c->name.empty() ? "-" : c->name.c_str(),
                 c->running ? std::to_string(c->init).c_str() : "-", status.c_str(), cmd.c_str());
        out += line;
    }
    queue(cl, out);
}

void do_logs(Client& cl, const Request& rq) {
    std::string err;
    Container* c = find(rq.get("ref"), err);
    if (!c) return reply_err(cl, err);
    std::ifstream f(c->dir + "/log", std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    reply_ok(cl);
    queue(cl, ss.str());
    if (rq.get("follow") == "1" && c->running) {
        cl.mode = Client::LOGS;  // stay connected, new output is appended as it arrives
        cl.c = c;
        cl.close_after_flush = false;
    }
}

void do_attach(Client& cl, const Request& rq) {
    std::string err;
    Container* c = find(rq.get("ref"), err);
    if (!c) return reply_err(cl, err);
    if (!c->running) return reply_err(cl, "container is not running");
    reply_ok(cl);
    cl.mode = Client::ATTACH;  // from now on this socket speaks the attach protocol
    cl.c = c;
    cl.close_after_flush = false;
}

void do_kill(Client& cl, const Request& rq) {
    std::string err;
    Container* c = find(rq.get("ref"), err);
    if (!c) return reply_err(cl, err);
    if (!c->running) return reply_err(cl, "container is not running");
    int sig = atoi(rq.get("sig", "0").c_str());
    int timeout = atoi(rq.get("timeout", "5").c_str());
    if (rq.get("force") == "1") {
        kill(c->init, SIGKILL);  // SIGKILL to PID 1 destroys the whole PID namespace
    } else if (sig > 0) {
        kill(c->init, sig);
    } else {
        // Graceful stop: SIGTERM now, SIGKILL after the timeout.
        kill(c->init, SIGTERM);
        if (timeout <= 0) kill(c->init, SIGKILL);
        else c->kill_deadline = time(nullptr) + timeout;
    }
    reply_ok(cl);
}

void do_rm(Client& cl, const Request& rq) {
    std::string err;
    Container* c = find(rq.get("ref"), err);
    if (!c) return reply_err(cl, err);
    if (c->running) return reply_err(cl, "container is running; kill it first");
    std::error_code ec;
    std::filesystem::remove_all(c->dir, ec);
    for (auto it = g_containers.begin(); it != g_containers.end(); ++it)
        if (it->get() == c) {
            g_containers.erase(it);
            break;
        }
    reply_ok(cl);
}

void handle_attach_input(Client& cl) {
    size_t off = 0;
    while (cl.in.size() - off >= 3) {
        char type = cl.in[off];
        size_t len = ((unsigned char)cl.in[off + 1] << 8) | (unsigned char)cl.in[off + 2];
        if (cl.in.size() - off - 3 < len) break;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(cl.in.data()) + off + 3;
        if (cl.c && cl.c->running) {
            if (type == FRAME_DATA && cl.c->pending_in.size() < (1u << 20)) {
                cl.c->pending_in.append(reinterpret_cast<const char*>(p), len);
            } else if (type == FRAME_RESIZE && len == 4) {
                winsize ws{};
                ws.ws_row = (unsigned short)((p[0] << 8) | p[1]);
                ws.ws_col = (unsigned short)((p[2] << 8) | p[3]);
                ioctl(cl.c->master, TIOCSWINSZ, &ws);  // kernel sends SIGWINCH to the app
            }
        }
        off += 3 + len;
    }
    cl.in.erase(0, off);
    if (cl.c) flush_pty(cl.c);
}

void handle_request_line(Client& cl, const std::string& line) {
    Request rq = parse_request(line);
    cl.close_after_flush = true;  // one-shot by default; attach/logs -f override
    if (rq.verb == "RUN") do_run(cl, rq);
    else if (rq.verb == "LIST") do_list(cl);
    else if (rq.verb == "LOGS") do_logs(cl, rq);
    else if (rq.verb == "ATTACH") do_attach(cl, rq);
    else if (rq.verb == "KILL") do_kill(cl, rq);
    else if (rq.verb == "RM") do_rm(cl, rq);
    else if (rq.verb == "PING") reply_ok(cl);
    else reply_err(cl, "unknown command: " + rq.verb);
}

void handle_client_input(Client& cl) {
    if (cl.mode == Client::ATTACH) return handle_attach_input(cl);
    if (cl.mode == Client::LOGS) {
        cl.in.clear();
        return;
    }
    size_t nl = cl.in.find('\n');
    if (nl == std::string::npos) {
        if (cl.in.size() > (1u << 20)) cl.close_after_flush = true;
        return;
    }
    std::string line = cl.in.substr(0, nl);
    cl.in.erase(0, nl + 1);
    handle_request_line(cl, line);
    if (cl.mode == Client::ATTACH && !cl.in.empty()) handle_attach_input(cl);
}

void reap_children() {
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0)
        for (auto& c : g_containers)
            if (c->shim == p && c->running) container_finished(c.get(), status);
}

int make_listener(const std::string& path) {
    int probe = connect_unix(path);
    if (probe >= 0) {
        close(probe);
        die("daemon is already running (%s)", path.c_str());
    }
    unlink(path.c_str());  // stale socket from a crashed daemon
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) die("socket path too long: %s", path.c_str());
    strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(fd, 64) < 0)
        die("cannot listen on %s: %s", path.c_str(), strerror(errno));
    chmod(path.c_str(), 0600);  // only the owner may talk to the daemon
    return fd;
}

}  // namespace

int run_daemon(bool detach) {
    mkdir_p(home_dir() + "/containers");
    int listener = make_listener(socket_path());

    if (detach) {
        pid_t pid = fork();
        if (pid < 0) die("fork: %s", strerror(errno));
        if (pid > 0) {
            printf("daemon started (pid %d), log: %s/daemon.log\n", pid, home_dir().c_str());
            _exit(0);
        }
        setsid();
        int dn = open("/dev/null", O_RDONLY);
        int lg = open((home_dir() + "/daemon.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        dup2(dn, 0);
        dup2(lg, 1);
        dup2(lg, 2);
    }

    if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) < 0) die("pipe");
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGCHLD, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "daemon listening on %s\n", socket_path().c_str());

    enum Kind { LISTEN, WAKE, MASTER, CLIENT };
    struct Tag {
        Kind kind;
        Container* c;
        int fd;
    };

    while (!g_stop) {
        std::vector<pollfd> pfds;
        std::vector<Tag> tags;
        auto add = [&](int fd, short ev, Tag t) {
            pfds.push_back({fd, ev, 0});
            tags.push_back(t);
        };
        add(listener, POLLIN, {LISTEN, nullptr, listener});
        add(g_wake[0], POLLIN, {WAKE, nullptr, g_wake[0]});
        bool deadlines = false;
        for (auto& c : g_containers) {
            if (c->running && c->master >= 0)
                add(c->master, POLLIN | (c->pending_in.empty() ? 0 : POLLOUT), {MASTER, c.get(), c->master});
            deadlines |= c->kill_deadline > 0;
        }
        for (auto& [fd, cl] : g_clients)
            add(fd, POLLIN | (cl.out.empty() ? 0 : POLLOUT), {CLIENT, nullptr, fd});

        int n = poll(pfds.data(), pfds.size(), deadlines ? 200 : -1);
        if (n < 0 && errno != EINTR) break;

        for (size_t i = 0; n > 0 && i < pfds.size(); i++) {
            short re = pfds[i].revents;
            if (!re) continue;
            Tag t = tags[i];
            if (t.kind == LISTEN) {
                int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd >= 0) g_clients[fd].fd = fd;
            } else if (t.kind == WAKE) {
                char buf[64];
                while (read(g_wake[0], buf, sizeof buf) > 0) {}
                reap_children();
            } else if (t.kind == MASTER) {
                if (t.c->master != t.fd) continue;  // closed earlier in this round
                if (re & POLLOUT) flush_pty(t.c);
                if (re & (POLLIN | POLLHUP | POLLERR)) {
                    if (drain_master(t.c)) {  // slave side is gone
                        close(t.c->master);
                        t.c->master = -1;
                    }
                }
            } else {
                auto it = g_clients.find(t.fd);
                if (it == g_clients.end()) continue;
                Client& cl = it->second;
                if (re & POLLOUT) {
                    ssize_t w = send(cl.fd, cl.out.data(), cl.out.size(), MSG_NOSIGNAL);
                    if (w > 0) cl.out.erase(0, (size_t)w);
                    else if (w < 0 && errno != EAGAIN && errno != EINTR) cl.close_after_flush = true, cl.out.clear();
                }
                if (re & POLLIN) {
                    char buf[8192];
                    ssize_t r = recv(cl.fd, buf, sizeof buf, 0);
                    if (r > 0) {
                        cl.in.append(buf, (size_t)r);
                        handle_client_input(cl);
                    } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                        cl.close_after_flush = true;  // peer went away
                        cl.out.clear();
                    }
                } else if (re & (POLLHUP | POLLERR)) {
                    cl.close_after_flush = true;
                    cl.out.clear();
                }
            }
        }

        // graceful stop timeouts
        time_t now = time(nullptr);
        for (auto& c : g_containers)
            if (c->running && c->kill_deadline > 0 && now >= c->kill_deadline) {
                kill(c->init, SIGKILL);
                c->kill_deadline = 0;
            }

        // close finished clients (after their output has been flushed)
        for (auto it = g_clients.begin(); it != g_clients.end();) {
            if (it->second.close_after_flush && it->second.out.empty()) {
                close(it->first);
                it = g_clients.erase(it);
            } else {
                ++it;
            }
        }
    }
    fprintf(stderr, "daemon shutting down\n");
    for (auto& c : g_containers)
        if (c->running) {
            kill(c->init, SIGKILL);
            int st;
            waitpid(c->shim, &st, 0);
        }
    unlink(socket_path().c_str());
    return 0;
}

}  // namespace rt
