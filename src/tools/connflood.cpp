// tools/connflood.cpp — client-generation program for the Bonus (§6.9).
//
// Creates and maintains N simultaneous, established, idle TCP connections to
// the Exchange Server. No application-level data is ever exchanged.
//
//   ./connflood <host> <port> <count> [--src a,b,c] [--batch N] [--report N]
//
//   --src     comma-separated local source addresses to bind and round-robin.
//             Needed above ~64k connections: one (src-ip, dst-ip, dst-port)
//             triple has only one ephemeral port range to draw from.
//   --batch   how many connects to issue before draining completions (default 256).
//   --report  progress interval (default 5000).
//
// On the first hard failure it stops, prints the errno and the exact count it
// reached — that number is the answer to Bonus question 1.
//
// Build:  c++ -std=c++17 -O2 -o connflood tools/connflood.cpp

#include <sys/types.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl != -1 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) != -1;
}

// Raise RLIMIT_NOFILE for this process to its hard maximum. The generator needs
// as many descriptors as the server does — on loopback every connection costs
// two sockets on this machine.
static void raise_fd_limit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) { perror("getrlimit"); return; }
    rlim_t old = rl.rlim_cur;
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) perror("setrlimit");
    getrlimit(RLIMIT_NOFILE, &rl);
    std::fprintf(stderr, "[gen] RLIMIT_NOFILE %llu -> %llu (max %llu)\n",
                 (unsigned long long)old,
                 (unsigned long long)rl.rlim_cur,
                 (unsigned long long)rl.rlim_max);
}

static double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: %s <host> <port> <count> [--src a,b,c] [--batch N] [--report N]\n",
            argv[0]);
        return 2;
    }

    const char* host = argv[1];
    const std::uint16_t port = (std::uint16_t)std::atoi(argv[2]);
    const long target = std::atol(argv[3]);

    std::vector<std::string> srcs;
    long batch = 256, report = 5000;

    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--src") && i + 1 < argc) {
            std::string s = argv[++i], cur;
            for (char c : s) {
                if (c == ',') { if (!cur.empty()) srcs.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) srcs.push_back(cur);
        } else if (!std::strcmp(argv[i], "--batch") && i + 1 < argc) {
            batch = std::atol(argv[++i]);
        } else if (!std::strcmp(argv[i], "--report") && i + 1 < argc) {
            report = std::atol(argv[++i]);
        }
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, nullptr, _IONBF, 0);

    raise_fd_limit();

    struct sockaddr_in dst;
    std::memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        std::fprintf(stderr, "[gen] bad host %s\n", host);
        return 1;
    }

    std::vector<struct sockaddr_in> binds;
    for (const std::string& s : srcs) {
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port   = 0;                 // let the kernel choose the port
        if (inet_pton(AF_INET, s.c_str(), &a.sin_addr) != 1) {
            std::fprintf(stderr, "[gen] bad source address %s\n", s.c_str());
            return 1;
        }
        binds.push_back(a);
    }
    if (!binds.empty())
        std::fprintf(stderr, "[gen] round-robin over %zu source addresses\n", binds.size());

    int kq = kqueue();
    if (kq < 0) { perror("kqueue"); return 1; }

    std::vector<int> fds;
    fds.reserve((size_t)target);

    long issued = 0, established = 0, pending = 0;
    const char* stop_reason = nullptr;
    int stop_errno = 0;
    const double t0 = now_s();

    std::vector<struct kevent> evs(1024);

    while (!g_stop && established < target && !stop_reason) {
        // --- issue a batch of non-blocking connects ---
        while (pending < batch && issued < target && !stop_reason) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                stop_errno = errno;
                stop_reason = (errno == EMFILE) ? "socket(): EMFILE (per-process fd limit)"
                            : (errno == ENFILE) ? "socket(): ENFILE (system-wide file table full)"
                            : "socket() failed";
                break;
            }
            if (!binds.empty()) {
                const struct sockaddr_in& b = binds[(size_t)(issued % (long)binds.size())];
                if (bind(fd, (const struct sockaddr*)&b, sizeof b) < 0) {
                    stop_errno = errno;
                    stop_reason = (errno == EADDRNOTAVAIL)
                        ? "bind(): EADDRNOTAVAIL (ephemeral ports exhausted for this source address)"
                        : "bind() failed";
                    close(fd);
                    break;
                }
            }
            set_nonblocking(fd);

            int r = connect(fd, (const struct sockaddr*)&dst, sizeof dst);
            if (r == 0) {                       // loopback usually completes at once
                fds.push_back(fd);
                ++issued; ++established;
            } else if (errno == EINPROGRESS) {
                struct kevent ev;
                EV_SET(&ev, (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
                kevent(kq, &ev, 1, nullptr, 0, nullptr);
                fds.push_back(fd);
                ++issued; ++pending;
            } else {
                stop_errno = errno;
                stop_reason =
                      (errno == EADDRNOTAVAIL) ? "connect(): EADDRNOTAVAIL (ephemeral port range exhausted)"
                    : (errno == EADDRINUSE)    ? "connect(): EADDRINUSE (4-tuple space exhausted)"
                    : (errno == ECONNREFUSED)  ? "connect(): ECONNREFUSED (server gone or listen backlog overflowed)"
                    : (errno == ETIMEDOUT)     ? "connect(): ETIMEDOUT"
                    : "connect() failed";
                close(fd);
                fds.pop_back();
                break;
            }

            if (report > 0 && established % report == 0 && established > 0)
                std::fprintf(stderr, "[gen] established=%ld pending=%ld t=%.1fs\n",
                             established, pending, now_s() - t0);
        }

        if (pending == 0) continue;

        // --- drain completions ---
        struct timespec ts{2, 0};
        int n = kevent(kq, nullptr, 0, evs.data(), (int)evs.size(), &ts);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("kevent");
            break;
        }
        if (n == 0 && pending > 0) {
            std::fprintf(stderr, "[gen] warning: %ld connects still pending after 2s\n", pending);
        }
        for (int i = 0; i < n; ++i) {
            int fd = (int)evs[i].ident;
            int soerr = 0; socklen_t sl = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);

            struct kevent ev;
            EV_SET(&ev, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            kevent(kq, &ev, 1, nullptr, 0, nullptr);
            --pending;

            if (soerr == 0) {
                ++established;
            } else {
                stop_errno = soerr;
                stop_reason = "asynchronous connect failed";
            }
        }
    }

    const double elapsed = now_s() - t0;
    std::fprintf(stderr,
        "\n[gen] ===== summary =====\n"
        "[gen] requested            : %ld\n"
        "[gen] established          : %ld\n"
        "[gen] sockets held open    : %zu\n"
        "[gen] elapsed              : %.2f s (%.0f conn/s)\n",
        target, established, fds.size(), elapsed,
        elapsed > 0 ? established / elapsed : 0.0);

    if (stop_reason)
        std::fprintf(stderr, "[gen] STOPPED: %s (errno %d: %s)\n",
                     stop_reason, stop_errno, std::strerror(stop_errno));
    else
        std::fprintf(stderr, "[gen] target reached\n");

    std::fprintf(stderr,
        "[gen] holding connections open and idle. Take measurements now.\n"
        "[gen] press Ctrl-C to release.\n");

    while (!g_stop) pause();

    std::fprintf(stderr, "[gen] closing %zu descriptors...\n", fds.size());
    for (int fd : fds) close(fd);
    close(kq);
    return stop_reason ? 1 : 0;
}
