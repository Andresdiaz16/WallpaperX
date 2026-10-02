// SPDX-License-Identifier: GPL-2.0-or-later

// What test_isolation_wrapper runs inside and beside a nested isolated_run.sh, where a shell has no
// portable way to do it:
//
//   wallpaperx-wrapper-probe listen PORT DIR  -- listen on 127.0.0.1:PORT, write DIR/ready once
//                                               listening, accept until DIR/stop exists (30 s at most)
//   wallpaperx-wrapper-probe connect PORT     -- exit 0 if 127.0.0.1:PORT accepts, 1 if refused, 2 otherwise
//   wallpaperx-wrapper-probe crash            -- die of SIGSEGV, marked undumpable so the host's crash
//                                               collector records nothing

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

sockaddr_in loopback(const char* port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(std::atoi(port)));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

int listenOn(const char* port, const std::string& dir) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const sockaddr_in address = loopback(port);
    if (fd < 0 || ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 || ::listen(fd, 8) != 0) {
        std::fprintf(stderr, "wallpaperx-wrapper-probe: cannot listen on 127.0.0.1:%s: %s\n", port, std::strerror(errno));
        return 2;
    }
    std::ofstream(dir + "/ready") << "listening\n";
    for (int i = 0; i < 300 && !std::filesystem::exists(dir + "/stop"); ++i) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 100) > 0) {
            const int client = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (client >= 0) ::close(client);
        }
    }
    ::close(fd);
    return 0;
}

int connectTo(const char* port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const sockaddr_in address = loopback(port);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0) {
        ::close(fd);
        return 0;
    }
    const int error = errno;
    std::fprintf(stderr, "wallpaperx-wrapper-probe: connect to 127.0.0.1:%s: %s\n", port, std::strerror(error));
    if (fd >= 0) ::close(fd);
    return error == ECONNREFUSED ? 1 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "listen" && argc == 4) return listenOn(argv[2], argv[3]);
    if (mode == "connect" && argc == 3) return connectTo(argv[2]);
    if (mode == "crash" && argc == 2) {
        ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        std::signal(SIGSEGV, SIG_DFL);
        std::raise(SIGSEGV);
        return 3;
    }
    std::fprintf(stderr, "usage: wallpaperx-wrapper-probe listen PORT DIR | connect PORT | crash\n");
    return 2;
}
