#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

static std::string readSocketPath() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    std::string path = std::string(home) + "/.cache/gmix/socket";
    std::ifstream in(path);
    if (!in) return {};
    std::string s; std::getline(in, s); return s;
}

int mainClient() {
    std::string sock = readSocketPath();
    if (sock.empty()) {
        std::fprintf(stderr, "gmix: no layer socket path found in ~/.cache/gmix/socket\n");
        return 1;
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr; std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (sock.size() >= sizeof(addr.sun_path)) { std::fprintf(stderr, "socket path too long\n"); return 1; }
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path)-1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) { perror("connect"); return 1; }
    char buf[1024];
    while (true) {
        ssize_t r = ::recv(fd, buf, sizeof(buf)-1, 0);
        if (r <= 0) break;
        buf[r] = '\0';
        std::fputs(buf, stdout);
    }
    ::close(fd);
    return 0;
}
