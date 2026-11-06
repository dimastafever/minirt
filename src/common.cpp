#include "common.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rt {

std::string home_dir() {
    if (const char* h = getenv("MINIRT_HOME"); h && *h) return h;
    if (const char* h = getenv("HOME"); h && *h) return std::string(h) + "/.minirt";
    return "/tmp/minirt-" + std::to_string(getuid());
}

std::string socket_path() { return home_dir() + "/daemon.sock"; }

void mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < path.size()) cur += path[i];
    }
}

void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-_./:", c)) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urldec(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

bool read_line(int fd, std::string& line) {
    line.clear();
    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return !line.empty();
        if (c == '\n') return true;
        line += c;
    }
}

int connect_unix(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return -1;
    strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool open_pty(int& master, int& slave) {
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return false;
    char name[128];
    if (grantpt(master) < 0 || unlockpt(master) < 0 || ptsname_r(master, name, sizeof name) != 0) {
        close(master);
        return false;
    }
    slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        close(master);
        return false;
    }
    return true;
}

bool send_frame(int fd, char type, const void* data, size_t len) {
    std::string buf;
    buf += type;
    buf += (char)((len >> 8) & 0xff);
    buf += (char)(len & 0xff);
    buf.append(static_cast<const char*>(data), len);
    return write_all(fd, buf.data(), buf.size());
}

}  // namespace rt
