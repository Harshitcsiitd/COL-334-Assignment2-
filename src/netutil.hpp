// netutil.hpp — thin wrappers over the raw POSIX socket / kqueue API.
#pragma once

#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

inline bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1) { perror("fcntl F_GETFL"); return false; }
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1) {   // OR into existing flags
        perror("fcntl F_SETFL");
        return false;
    }
    return true;
}

// socket -> setsockopt -> bind -> nonblocking -> listen. Returns -1 on failure.
inline int make_listen_socket(const char* ip, std::uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    // Lets the server rebind immediately while old connections sit in
    // TIME_WAIT. It does not permit two live listeners on the same endpoint.
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0)
        perror("setsockopt SO_REUSEADDR");

    struct sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        std::fprintf(stderr, "bad listen address: %s\n", ip);
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (!set_nonblocking(fd)) { close(fd); return -1; }
    if (listen(fd, 4096) < 0) { //initially it was SOMAXCONN but for bonus we need to change it to 4096
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

inline int kq_change(int kq, int fd, std::int16_t filter, std::uint16_t flags) {
    struct kevent ev;
    EV_SET(&ev, static_cast<uintptr_t>(fd), filter, flags, 0, 0, nullptr);
    int r = kevent(kq, &ev, 1, nullptr, 0, nullptr);
    if (r < 0 && errno != ENOENT) perror("kevent (change)");
    return r;
}

inline ssize_t safe_recv(int fd, char* buf, std::size_t n) {
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

inline ssize_t safe_send(int fd, const char* buf, std::size_t n) {
    for (;;) {
        ssize_t r = send(fd, buf, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}
