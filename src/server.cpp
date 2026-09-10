// server.cpp — Exchange Server.
// Single-threaded, non-blocking sockets, one kqueue event loop.
//
//   ./exchange_server [ip] [port] [-v]
//   -v : log every recv() with its byte count and payload (Experiment 3)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book.hpp"
#include "framing.hpp"
#include "netutil.hpp"
#include "protocol.hpp"

static bool g_verbose = false;

enum class Role { Unknown, Trader, MarketData };

struct Conn {
    int         fd       = -1;
    int         owner_id = 0;      // logical id; never reused, outlives the fd
    Role        role     = Role::Unknown;
    std::string username;
    bool        subs[2]  = {false, false};
    LineBuffer  in;
    OutBuffer   out;
    bool        write_enabled = false;   // is EVFILT_WRITE currently enabled?
    bool        closing       = false;   // drain `out`, then close (QUIT)
    bool        dead          = false;   // closed; awaiting reap
};

class Server {
public:
    Server(const char* ip, std::uint16_t port) : ip_(ip), port_(port) {}

    bool init() {
        lfd_ = make_listen_socket(ip_.c_str(), port_);
        if (lfd_ < 0) return false;
        kq_ = kqueue();
        if (kq_ < 0) { perror("kqueue"); return false; }
        kq_change(kq_, lfd_, EVFILT_READ, EV_ADD | EV_ENABLE);
        std::fprintf(stderr, "[server] listening on %s:%u (fd=%d, kq=%d)\n",
                     ip_.c_str(), static_cast<unsigned>(port_), lfd_, kq_);
        return true;
    }

    void run() {
        struct kevent evs[64];
        for (;;) {
            // NULL timeout: the process sleeps here whenever nothing is ready.
            // An idle client therefore costs zero CPU and blocks nobody
            // (Experiment 4: procstat -k shows the stack parked in kevent).
            int n = kevent(kq_, nullptr, 0, evs, 64, nullptr);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("kevent (wait)");
                break;
            }
            for (int i = 0; i < n; ++i) {
                int fd = static_cast<int>(evs[i].ident);
                if (fd == lfd_) { on_accept(); continue; }
                if (evs[i].filter == EVFILT_READ)       on_readable(fd, evs[i]);
                else if (evs[i].filter == EVFILT_WRITE) on_writable(fd);
            }
            reap();     // close fds and erase Conns only here, never mid-loop
        }
    }

private:
    // ------------------------------------------------------------- accepting
    void on_accept() {
        for (;;) {   // drain the backlog: one wakeup may cover several connects
            struct sockaddr_in peer;
            socklen_t plen = sizeof peer;
            int cfd = accept(lfd_, reinterpret_cast<struct sockaddr*>(&peer), &plen);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                if (errno == EMFILE || errno == ENFILE) {
                    std::fprintf(stderr, "[server] accept: out of file descriptors\n");
                    break;
                }
                perror("accept");
                break;
            }
            set_nonblocking(cfd);

            Conn c;
            c.fd = cfd;
            c.owner_id = next_owner_++;
            conns_.emplace(cfd, std::move(c));

            // Registered once each, at accept time; write interest is later
            // toggled with EV_ENABLE/EV_DISABLE rather than add/delete.
            kq_change(kq_, cfd, EVFILT_READ,  EV_ADD | EV_ENABLE);
            kq_change(kq_, cfd, EVFILT_WRITE, EV_ADD | EV_DISABLE);

            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof ipbuf);
            std::fprintf(stderr, "[server] accepted fd=%d owner=%d from %s:%u\n",
                         cfd, next_owner_ - 1, ipbuf, ntohs(peer.sin_port));
        }
    }

    // --------------------------------------------------------------- reading
    void on_readable(int fd, const struct kevent& ev) {
        Conn* cp = find_live(fd);
        if (!cp) return;
        Conn& c = *cp;

        char tmp[4096];
        for (;;) {
            ssize_t n = recv(fd, tmp, sizeof tmp, 0);
            if (n > 0) {
                if (g_verbose)
                    std::fprintf(stderr, "[recv] fd=%d n=%zd data=%.*s\n",
                                 fd, n, static_cast<int>(n), tmp);
                c.in.append(tmp, static_cast<std::size_t>(n));

                std::string line;
                for (;;) {
                    LineStatus st = c.in.next_line(line);
                    if (st == LineStatus::NeedMore) break;
                    if (st == LineStatus::TooLong) {
                        queue_out(c, msg_error("line too long"));
                        c.closing = true;
                        try_flush(c);
                        return;
                    }
                    handle_line(c, line);
                    if (c.dead || c.closing) return;
                }
                continue;                       // socket may hold more bytes
            }
            if (n == 0) {                       // orderly FIN from the peer
                std::fprintf(stderr, "[server] fd=%d: peer closed (FIN, recv==0)\n", fd);
                close_conn(c);
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Nothing left right now. If kqueue also reported EOF and the
                // stream is drained, finish the teardown.
                if (ev.flags & EV_EOF) close_conn(c);
                return;
            }
            if (errno == ECONNRESET) {          // abrupt: peer sent RST (Exp. 6)
                std::fprintf(stderr, "[server] fd=%d: connection reset by peer (RST, ECONNRESET)\n", fd);
                close_conn(c);
                return;
            }
            std::fprintf(stderr, "[server] fd=%d: recv error: %s\n", fd, std::strerror(errno));
            close_conn(c);
            return;
        }
    }

    void on_writable(int fd) {
        Conn* cp = find_live(fd);
        if (cp) try_flush(*cp);
    }

    // --------------------------------------------------------------- writing
    // Every server->client byte goes through here. No bare send() elsewhere.
    void queue_out(Conn& c, const std::string& msg) {
        if (c.dead) return;
        c.out.push(msg);
        if (c.out.pending() > kMaxOutBytes) {   // unbounded slow reader
            std::fprintf(stderr,
                         "[server] fd=%d: output backlog %zu bytes exceeds cap, dropping client\n",
                         c.fd, c.out.pending());
            c.out.clear();
            close_conn(c);
            return;
        }
        try_flush(c);
    }

    void try_flush(Conn& c) {
        if (c.dead) return;
        while (!c.out.empty()) {
            ssize_t n = send(c.fd, c.out.ptr(), c.out.len(), 0);
            if (n > 0) { c.out.consume(static_cast<std::size_t>(n)); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // The socket send buffer is full: this is TCP backpressure.
                // Keep the remainder queued, ask kqueue to tell us when the
                // peer drains, and go back to serving everyone else. On a
                // blocking socket this send() would sleep and freeze the whole
                // server for one slow reader (Experiment 7).
                std::fprintf(stderr,
                    "[flush] fd=%d send() EAGAIN, %zu bytes still queued, enabling EVFILT_WRITE\n",
                    c.fd, c.out.pending());
                if (!c.write_enabled) {
                    kq_change(kq_, c.fd, EVFILT_WRITE, EV_ENABLE);
                    c.write_enabled = true;
                }
                return;
            }
            std::fprintf(stderr, "[server] fd=%d: send error: %s\n",
                         c.fd, std::strerror(errno));
            close_conn(c);                       // EPIPE / ECONNRESET
            return;
        }
        if (c.write_enabled) {
            kq_change(kq_, c.fd, EVFILT_WRITE, EV_DISABLE);
            c.write_enabled = false;
        }
        if (c.closing) close_conn(c);            // QUIT: buffer drained, done
    }

    // -------------------------------------------------------------- dispatch
    void handle_line(Conn& c, const std::string& line) {
        auto tk = split_ws(line);
        if (tk.empty()) return;                  // ignore blank lines
        const std::string& v = tk[0];

        if (v == "LOGIN")        return cmd_login(c, tk);
        if (v == "BUY")          return cmd_order(c, tk, Side::Buy);
        if (v == "SELL")         return cmd_order(c, tk, Side::Sell);
        if (v == "CANCEL")       return cmd_cancel(c, tk);
        if (v == "SUBSCRIBE")    return cmd_subscribe(c, tk, true);
        if (v == "UNSUBSCRIBE")  return cmd_subscribe(c, tk, false);
        if (v == "QUIT")         return cmd_quit(c);
        queue_out(c, msg_error("unknown command"));
    }

    void cmd_login(Conn& c, const std::vector<std::string>& tk) {
        if (tk.size() != 2)            return queue_out(c, msg_error("bad arguments"));
        if (c.role == Role::MarketData)
            return queue_out(c, msg_error("not permitted for this client type"));
        if (c.role == Role::Trader)    return queue_out(c, msg_error("already logged in"));
        if (usernames_.count(tk[1]))   return queue_out(c, msg_error("duplicate username"));

        c.role = Role::Trader;                    // role fixed by first command
        c.username = tk[1];
        usernames_.insert(tk[1]);
        owner_fd_[c.owner_id] = c.fd;             // "is this trader connected?"
        queue_out(c, msg_ok());
    }

    void cmd_order(Conn& c, const std::vector<std::string>& tk, Side side) {
        if (c.role == Role::MarketData)
            return queue_out(c, msg_error("not permitted for this client type"));
        if (c.role == Role::Unknown)   return queue_out(c, msg_error("not logged in"));
        if (tk.size() != 4)            return queue_out(c, msg_error("bad arguments"));

        int inst = instrument_index(tk[1]);
        if (inst < 0)                  return queue_out(c, msg_error("unknown instrument"));
        std::uint32_t qty = 0, price = 0;
        if (!parse_u32(tk[2], kMinValue, kMaxValue, qty))
            return queue_out(c, msg_error("bad quantity"));
        if (!parse_u32(tk[3], kMinValue, kMaxValue, price))
            return queue_out(c, msg_error("bad price"));

        // Only past every validation gate do we touch the exchange, so a
        // rejected order never consumes an order id.
        std::vector<Trade> trades;
        std::uint32_t id = exch_.submit(side, inst, qty, price, c.owner_id, trades);
        queue_out(c, msg_order_accepted(id));     // ACCEPTED precedes executions
        for (const Trade& t : trades) publish(t);
    }

    void cmd_cancel(Conn& c, const std::vector<std::string>& tk) {
        if (c.role == Role::MarketData)
            return queue_out(c, msg_error("not permitted for this client type"));
        if (c.role == Role::Unknown)   return queue_out(c, msg_error("not logged in"));
        if (tk.size() != 2)            return queue_out(c, msg_error("bad arguments"));

        std::uint32_t id = 0;
        if (!parse_u32(tk[1], 0u, kMaxOrderId, id))
            return queue_out(c, msg_error("bad order id"));

        std::string err;
        if (exch_.cancel(id, c.owner_id, err)) queue_out(c, msg_order_cancelled(id));
        else                                   queue_out(c, msg_error(err));
    }

    void cmd_subscribe(Conn& c, const std::vector<std::string>& tk, bool on) {
        if (c.role == Role::Trader)
            return queue_out(c, msg_error("not permitted for this client type"));
        if (tk.size() != 2)            return queue_out(c, msg_error("bad arguments"));
        int inst = instrument_index(tk[1]);
        if (inst < 0)                  return queue_out(c, msg_error("unknown instrument"));

        c.role = Role::MarketData;                // Unknown -> MarketData
        c.subs[inst] = on;                        // idempotent, still OK
        queue_out(c, msg_ok());
    }

    void cmd_quit(Conn& c) {
        c.closing = true;
        try_flush(c);        // flush what is pending, then close_conn -> FIN
    }

    // ------------------------------------------------------------ publishing
    void publish(const Trade& t) {
        auto bit = owner_fd_.find(t.buyer_owner);
        if (bit != owner_fd_.end()) {
            Conn* c = find_live(bit->second);
            if (c) queue_out(*c, msg_bought(t.inst, t.qty, t.price));
        }
        auto sit = owner_fd_.find(t.seller_owner);
        if (sit != owner_fd_.end()) {
            Conn* c = find_live(sit->second);
            if (c) queue_out(*c, msg_sold(t.inst, t.qty, t.price));
        }
        // A missing entry above is a trader who has disconnected: the trade
        // still happens and TRADE still goes out, only BOUGHT/SOLD is skipped.
        // Safe to iterate: close_conn never erases from conns_ (see reap()).
        for (auto& kv : conns_) {
            Conn& c = kv.second;
            if (c.role == Role::MarketData && c.subs[t.inst] && !c.closing && !c.dead)
                queue_out(c, msg_trade(t.inst, t.qty, t.price));
        }
    }

    // ------------------------------------------------------------- teardown
    // Marks dead and releases logical state now; the fd is closed and the Conn
    // erased in reap(), so no iterator is invalidated mid-event and no fd
    // number can be recycled while events for it are still in the batch.
    void close_conn(Conn& c) {
        if (c.dead) return;
        c.dead = true;
        if (!c.username.empty()) usernames_.erase(c.username);
        owner_fd_.erase(c.owner_id);
        shutdown(c.fd, SHUT_WR);          // send FIN; best effort (ENOTCONN ok)
        dead_.push_back(c.fd);
        std::fprintf(stderr, "[server] closing fd=%d owner=%d\n", c.fd, c.owner_id);
        // Resting orders are deliberately left in exch_ (handout §2.6).
    }

    void reap() {
        for (int fd : dead_) {
            close(fd);                    // close() also removes its kevents
            conns_.erase(fd);
        }
        dead_.clear();
    }

    Conn* find_live(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end() || it->second.dead) return nullptr;
        return &it->second;
    }

    static constexpr std::size_t kMaxOutBytes = 8u * 1024u * 1024u;

    std::string    ip_;
    std::uint16_t  port_;
    int            lfd_ = -1;
    int            kq_  = -1;
    int            next_owner_ = 1;

    std::unordered_map<int, Conn>   conns_;      // fd -> connection
    std::unordered_map<int, int>    owner_fd_;   // owner_id -> fd (connected only)
    std::unordered_set<std::string> usernames_;
    std::vector<int>                dead_;
    Exchange                        exch_;
};

int main(int argc, char** argv) {
    const char* ip = "127.0.0.1";
    std::uint16_t port = 5000;
    if (const char* e = getenv("SX_VERBOSE"); e && *e && e[0] != '0') g_verbose = true;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) { g_verbose = true; continue; }
        if (positional == 0)      { ip = argv[i]; positional++; }
        else if (positional == 1) { port = static_cast<std::uint16_t>(std::atoi(argv[i])); positional++; }
    }

    // Without this, writing to a peer that has gone away kills the process.
    std::signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, nullptr, _IONBF, 0);

    Server s(ip, port);
    if (!s.init()) return 1;
    s.run();
    return 0;
}
