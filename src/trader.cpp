// trader.cpp — Trader Client entry point.
//   ./trader_client <host> <port> <username>
// Then type protocol commands on stdin: BUY JNST 100 238, CANCEL 42, QUIT ...
#include <csignal>
#include <cstdio>
#include <string>

#include "client_common.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <host> <port> <username>\n", argv[0]);
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);

    int fd = connect_to(argv[1], argv[2]);
    if (!send_all(fd, std::string("LOGIN ") + argv[3] + "\n")) {
        std::fprintf(stderr, "login send failed\n");
        return 1;
    }
    client_loop(fd);
    return 0;
}
