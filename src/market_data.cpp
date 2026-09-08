// market_data.cpp — Market-Data Client entry point.
//   ./market_data_client <host> <port> <instrument> [<instrument> ...]
// Subscribes to each instrument given, then prints TRADE updates as they arrive.
#include <csignal>
#include <cstdio>
#include <string>

#include "client_common.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <host> <port> <instrument> [instrument ...]\n", argv[0]);
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);

    int fd = connect_to(argv[1], argv[2]);
    for (int i = 3; i < argc; ++i) {
        if (!send_all(fd, std::string("SUBSCRIBE ") + argv[i] + "\n")) {
            std::fprintf(stderr, "subscribe send failed\n");
            return 1;
        }
    }
    client_loop(fd);
    return 0;
}
