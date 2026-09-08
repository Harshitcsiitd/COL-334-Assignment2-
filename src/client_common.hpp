// client_common.hpp — shared client plumbing.
// Clients use blocking sockets plus select() to multiplex stdin and the socket;
// without the multiplexing a trader parked in getline() could never print an
// asynchronous BOUGHT/SOLD.
#pragma once

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "framing.hpp"

inline int connect_to(const char* host, const char* port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); std::exit(1); }

    struct sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<std::uint16_t>(std::atoi(port)));
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        std::fprintf(stderr, "bad server address: %s\n", host);
        std::exit(1);
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0) {
        perror("connect");
        std::exit(1);
    }
    return fd;
}

// Partial writes apply to clients too: one send() need not write everything.
inline bool send_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n > 0) { off += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

inline void client_loop(int fd) {
    // Not cosmetic: with stdout on a pipe (as experiment.py runs us), C++ would
    // switch to 4 KB block buffering and the experiment would see nothing.
    setvbuf(stdout, nullptr, _IONBF, 0);

    LineBuffer in;
    bool stdin_open = true;

    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        if (stdin_open) FD_SET(STDIN_FILENO, &rs);
        int mx = std::max(fd, STDIN_FILENO);

        if (select(mx + 1, &rs, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rs)) {
            std::string line;
            if (!std::getline(std::cin, line)) {       // EOF / Ctrl-D
                send_all(fd, "QUIT\n");
                shutdown(fd, SHUT_WR);                 // half-close: FIN out,
                stdin_open = false;                    // keep reading replies
            } else {
                if (!send_all(fd, line + "\n")) {
                    std::fprintf(stderr, "send failed\n");
                    break;
                }
            }
        }

        if (FD_ISSET(fd, &rs)) {
            char buf[4096];
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n == 0) { std::fprintf(stderr, "server closed connection\n"); break; }
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }
            in.append(buf, static_cast<std::size_t>(n));
            std::string msg;
            while (in.next_line(msg) == LineStatus::Ok)
                std::printf("%s\n", msg.c_str());
        }
    }
    close(fd);
}
