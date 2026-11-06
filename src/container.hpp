#pragma once
#include <sys/types.h>

#include <string>
#include <vector>

namespace rt {

struct ContainerConfig {
    std::string rootfs;                 // absolute path to an extracted rootfs
    std::vector<std::string> cmd;       // command and arguments
    std::vector<std::string> env;       // extra "KEY=VALUE"
    std::string hostname = "container";
    bool overlay = false;               // rootfs becomes a read-only lower layer
    std::string overlay_dir;            // where upper/work/merged live (if overlay)
    pid_t net_from = 0;                 // join user+net namespaces of this host pid
    bool ctty = false;                  // make fd 0 the controlling terminal (PTY mode)
};

struct Spawned {
    pid_t shim = 0;  // our child; wait for it to get the exit code
    pid_t init = 0;  // host pid of the process that is PID 1 inside the container
};
bool spawn_container(const ContainerConfig& cfg, int in, int out, int err, Spawned& sp,
                     std::string& error);
int exit_code_from_status(int status);

}  // namespace rt
