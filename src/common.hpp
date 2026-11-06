// Small helpers shared by the CLI, the daemon and the container core.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace rt {

std::string home_dir();
std::string socket_path();

void mkdir_p(const std::string& path);
[[noreturn]] void die(const char* fmt, ...);

std::string urlenc(const std::string& s);
std::string urldec(const std::string& s);
std::vector<std::string> split_ws(const std::string& s);

bool write_all(int fd, const void* buf, size_t n);
bool read_line(int fd, std::string& line);  // reads byte by byte, strips '\n'
int connect_unix(const std::string& path);  // -1 on failure
bool open_pty(int& master, int& slave);

constexpr char FRAME_DATA = 'D';
constexpr char FRAME_RESIZE = 'W';
bool send_frame(int fd, char type, const void* data, size_t len);

}  // namespace rt
