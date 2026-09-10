// tools/threaded_server.cpp — comparison artifact for Bonus questions 3-5 ONLY.
// NOT part of the graded Exchange Server. It exists so the thread-per-connection
// design can be measured against the kqueue design under identical load.
//
//   ./threaded_server <ip> <port> [stack_kb]
//
// One blocking accept loop; one detached thread per connection, each parked in a
// blocking recv() that never returns because the peer is idle. stack_kb lets you
// show how far shrinking the per-thread stack moves the ceiling (default: the
// system default stack).
//
// Build:  c++ -std=c++17 -O2 -o threaded_server tools/threaded_server.cpp -lpthread

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static volatile sig_atomic_t g_live = 0;
static size_t g_stack_bytes = 0;

static void* conn_thread(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[256];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);   // blocks forever on an idle peer
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
    }
    close(fd);
    __sync_fetch_and_sub(&g_live, 1);
    return nullptr;
}

int main(int argc, char** argv) {
    const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port  = (argc > 2) ? (uint16_t)atoi(argv[2]) : 5001;
    if (argc > 3) g_stack_bytes = (size_t)atol(argv[3]) * 1024;

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, nullptr, _IONBF, 0);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (bind(lfd, (struct sockaddr*)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 1024) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "[threaded] listening on %s:%u, stack=%s\n", ip, port,
            g_stack_bytes ? "custom" : "system default");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (g_stack_bytes) pthread_attr_setstacksize(&attr, g_stack_bytes);

    for (;;) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            fprintf(stderr, "[threaded] accept failed at %u live threads: %s\n",
                    (unsigned)g_live, strerror(errno));
            break;
        }
        pthread_t t;
        int rc = pthread_create(&t, &attr, conn_thread, (void*)(intptr_t)cfd);
        if (rc != 0) {
            // THIS is the measurement: the exact connection count at which a
            // thread-per-connection design stops scaling, and why.
            fprintf(stderr,
                    "[threaded] pthread_create FAILED at %u live threads: %s (rc=%d)\n",
                    (unsigned)g_live, strerror(rc), rc);
            close(cfd);
            break;
        }
        unsigned n = __sync_add_and_fetch(&g_live, 1);
        if (n % 250 == 0) fprintf(stderr, "[threaded] live threads: %u\n", n);
    }

    fprintf(stderr, "[threaded] stopped with %u live threads\n", (unsigned)g_live);
    pause();
    return 0;
}
