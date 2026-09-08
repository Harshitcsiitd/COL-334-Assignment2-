# Socket Exchange — C++ Implementation Guide

Target: FreeBSD 14.4, clang++ (`c++`), C++17, single-threaded `kqueue` event loop.

## File layout

```
src/
  framing.hpp      LineBuffer + OutBuffer     (header-only, ~90 lines)
  protocol.hpp     parsing + message builders (header-only, ~120 lines)
  book.hpp         Order / Trade / Exchange declarations
  book.cpp         matching engine            (~150 lines)
  netutil.hpp      socket helpers             (header-only, ~80 lines)
  server.cpp       Conn, dispatch, event loop (~450 lines)
  client_common.hpp connect + stdin/socket loop (~120 lines)
  trader.cpp       ~30 lines
  market_data.cpp  ~35 lines
Makefile
```

Build order: `framing` → `protocol` → `book` → `server` → clients. Each stage is testable
with `nc` before you move on.

---

# 1. `src/framing.hpp`

Two classes. Nothing here touches sockets — that separation is deliberate, it lets you
unit-test framing without a network.

## 1.1 `LineBuffer` — inbound framing

```cpp
enum class LineStatus { Ok, NeedMore, TooLong };

class LineBuffer {
    std::string buf_;
    static constexpr size_t kMaxLine = 65536;
public:
    void append(const char* p, size_t n);
    LineStatus next_line(std::string& out);
    void clear();
};
```

**`append(p, n)`** — `buf_.append(p, n);`. Called once per successful `recv()`.

**`next_line(out)`** — the heart of the framing requirement.

1. `size_t pos = buf_.find('\n');`
2. If `pos == std::string::npos`: if `buf_.size() > kMaxLine` return `TooLong`
   (caller replies `ERROR line too long` and closes); else return `NeedMore`.
3. `out.assign(buf_, 0, pos);`
4. `buf_.erase(0, pos + 1);`  ← the `+1` consumes the newline itself.
5. If `out` ends with `'\r'`, pop it. (`nc` and telnet send CRLF; without this every
   command silently fails and you'll lose an hour to it.)
6. Return `Ok`.

The caller always loops: `while (in.next_line(line) == Ok) handle_line(line);`
That single loop is what makes "multiple messages in one recv" work, and the fact that
`buf_` persists across calls is what makes "one message across several recvs" work.

## 1.2 `OutBuffer` — outbound queue with an offset

```cpp
class OutBuffer {
    std::string data_;
    size_t off_ = 0;
public:
    void push(const std::string& s);
    bool empty() const { return off_ >= data_.size(); }
    const char* ptr() const { return data_.data() + off_; }
    size_t len() const { return data_.size() - off_; }
    void consume(size_t n);
    size_t pending() const { return len(); }
};
```

**`consume(n)`** — `off_ += n;` then if `off_ == data_.size()` do `data_.clear(); off_ = 0;`,
else if `off_ > 4096` do `data_.erase(0, off_); off_ = 0;`.

Why the offset instead of `data_.erase(0, n)` every time: erasing the front of a string is
O(remaining), so a slow client that accumulates megabytes turns your writes quadratic.
This matters in Experiment 7 and is a good viva answer.

**`push`** — `data_ += s;` and cap it: if `pending() > 8*1024*1024`, mark the connection for
forced close. A market-data client that never reads must not be allowed to grow your heap
without bound. Mention this policy in your report.

---

# 2. `src/protocol.hpp`

## 2.1 Constants

```cpp
inline constexpr const char* kInstruments[2] = {"JNST", "IMCT"};
inline constexpr uint32_t kMinQty = 1, kMaxQty = 2147483647u;
inline constexpr uint32_t kMaxOrderId = 2147483647u;
```

Represent an instrument internally as `int` 0 or 1. Never carry strings into the book.

## 2.2 `std::vector<std::string> split_ws(const std::string& s)`

Split on runs of space/tab, skipping empties. Use an index walk, not `istringstream`
(faster, and you avoid locale surprises). Returns empty vector for a blank line — the
caller must treat that as "ignore" or `ERROR empty message`, your choice, but be
consistent.

## 2.3 `bool parse_u32(const std::string& s, uint32_t lo, uint32_t hi, uint32_t& out)`

This is the validator the graders will hammer. Be strict:

1. If `s.empty()` return false.
2. If `s.size() > 10` return false (prevents overflow before you even convert).
3. For every char: if `!(c >= '0' && c <= '9')` return false.
   This one loop rejects `-5`, `+7`, `2.5`, `1e3`, ` 42`, `42 `, `0x10`, and Unicode digits.
   Do **not** use `std::stoi`/`atoi` — `atoi("12abc")` returns 12 and `stoi` throws.
4. `unsigned long long v = strtoull(s.c_str(), nullptr, 10);`
5. `if (v < lo || v > hi) return false;`
6. `out = (uint32_t)v; return true;`

Leading zeros (`0100`): accept them, they're unambiguous decimal. Note the decision in
your README so it isn't read as an oversight.

Call it as `parse_u32(tok, 1, 2147483647u, qty)` for qty/price and
`parse_u32(tok, 0, 2147483647u, id)` for order IDs.

## 2.4 `int instrument_index(const std::string& s)`

Returns 0, 1, or -1. Exact match only — `jnst` is invalid.

## 2.5 Message builders

One function per server message. Each returns a `std::string` **including the trailing
`\n`**. Building the newline here rather than at call sites means you can never forget it.

```cpp
inline std::string msg_ok()                    { return "OK\n"; }
inline std::string msg_error(const std::string& r) { return "ERROR " + r + "\n"; }
inline std::string msg_order_accepted(uint32_t id);   // "ORDER_ACCEPTED 42\n"
inline std::string msg_order_cancelled(uint32_t id);
inline std::string msg_bought(int inst, uint32_t q, uint32_t p);
inline std::string msg_sold(int inst, uint32_t q, uint32_t p);
inline std::string msg_trade(int inst, uint32_t q, uint32_t p);
```

Use `std::to_string`. Reason strings should be short, lowercase, no newlines inside:
`bad quantity`, `bad price`, `unknown instrument`, `not logged in`, `duplicate username`,
`unknown command`, `not permitted for this client type`, `unknown order`,
`order not cancellable`, `bad arguments`.

---

# 3. `src/book.hpp` / `src/book.cpp`

## 3.1 Types

```cpp
enum class Side { Buy, Sell };

struct Order {
    uint32_t id;
    Side     side;
    int      inst;
    uint32_t price;
    uint32_t remaining;
    int      owner;     // logical trader id — NOT an fd
};

struct Trade {
    int      inst;
    uint32_t qty, price;
    int      buyer_owner, seller_owner;
};
```

`owner` being a logical id, not a file descriptor, is what makes §2.6 of the handout work
(orders outlive their client, and fds get reused). Assign each accepted connection a
monotonically increasing `owner_id` and never reuse it.

## 3.2 `class Exchange`

```cpp
class Exchange {
    uint32_t next_id_ = 0;
    std::unordered_map<uint32_t, Order> orders_;
    // books_[inst][side]: price -> FIFO of order ids resting at that price
    std::map<uint32_t, std::deque<uint32_t>> books_[2][2];
public:
    uint32_t submit(Side s, int inst, uint32_t qty, uint32_t price, int owner,
                    std::vector<Trade>& out);
    bool cancel(uint32_t id, int owner, std::string& err);
};
```

Because matching requires *exactly equal* price, you never need to scan a price range —
one `find(price)` in the opposite book is enough. `std::map` also keeps things ordered if
you later want to print the book for debugging.

## 3.3 `submit(...)`

```
id = next_id_++;
Order o{id, side, inst, price, qty, owner};

auto& opp = books_[inst][ side == Buy ? SELL_IDX : BUY_IDX ];
auto it = opp.find(price);

while (o.remaining > 0 && it != opp.end() && !it->second.empty()) {
    uint32_t other_id = it->second.front();
    Order& other = orders_.at(other_id);

    uint32_t q = std::min(o.remaining, other.remaining);
    o.remaining     -= q;
    other.remaining -= q;

    Trade t;
    t.inst = inst; t.qty = q; t.price = price;
    if (side == Side::Buy) { t.buyer_owner = owner;        t.seller_owner = other.owner; }
    else                   { t.buyer_owner = other.owner;  t.seller_owner = owner; }
    out.push_back(t);

    if (other.remaining == 0) {
        it->second.pop_front();
        orders_.erase(other_id);
    }
}
if (it != opp.end() && it->second.empty()) opp.erase(it);   // erase AFTER the loop

if (o.remaining > 0) {
    orders_.emplace(id, o);
    books_[inst][side_idx(side)][price].push_back(id);
}
return id;
```

Three traps:

- Do not erase the map entry inside the loop while `it` is still in use. Erase once,
  after.
- `Order& other = orders_.at(...)` is a reference into an `unordered_map`. `erase` on a
  *different* key doesn't invalidate it, but `orders_.emplace` at the end may rehash — so
  do the emplace last, after all references are dead. (Note: `unordered_map` references
  survive rehash; iterators don't. Still, keeping the emplace last avoids the question.)
- FIFO at a price level is your time-priority rule. Say so in the report.

The caller sends `ORDER_ACCEPTED <id>` **before** iterating `out`, so the ordering on the
wire is `ORDER_ACCEPTED`, then `BOUGHT`/`SOLD`, matching the handout's example.

## 3.4 `cancel(id, owner, err)`

1. `auto it = orders_.find(id);` if not found → `err = "unknown order"; return false;`
   (An order that fully executed is gone from `orders_`, so this correctly rejects it.)
2. If `it->second.owner != owner` → `err = "not your order"; return false;`
   The spec says "a previously submitted order" — enforcing ownership is the defensible
   reading. State the choice in your README.
3. Remove the id from `books_[inst][side][price]`: `std::find` on the deque then `erase`.
   Linear, but a price level is short; fine at this scale.
4. If that deque is now empty, erase the price key.
5. `orders_.erase(it); return true;`

---

# 4. `src/netutil.hpp`

## 4.1 `void set_nonblocking(int fd)`

```cpp
int fl = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, fl | O_NONBLOCK);
```
Check both for -1. Note you must OR into the *existing* flags, not assign.

## 4.2 `int make_listen_socket(const char* ip, uint16_t port)`

```
fd = socket(AF_INET, SOCK_STREAM, 0);
int yes = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
struct sockaddr_in a{};
a.sin_family = AF_INET;
a.sin_port   = htons(port);
inet_pton(AF_INET, ip, &a.sin_addr);        // "127.0.0.1"
bind(fd, (sockaddr*)&a, sizeof a);
set_nonblocking(fd);
listen(fd, SOMAXCONN);
return fd;
```

`SO_REUSEADDR` is what lets you restart the server immediately instead of waiting out
`TIME_WAIT` — you will restart it fifty times while testing. Understand *why* for the viva:
it permits binding while old connections linger in `TIME_WAIT`, and does not permit two
live listeners.

## 4.3 `void kq_change(int kq, int fd, int16_t filter, uint16_t flags)`

```cpp
struct kevent ev;
EV_SET(&ev, fd, filter, flags, 0, 0, nullptr);
kevent(kq, &ev, 1, nullptr, 0, nullptr);
```

Register each accepted fd **twice** at accept time:
- `EVFILT_READ` with `EV_ADD | EV_ENABLE`
- `EVFILT_WRITE` with `EV_ADD | EV_DISABLE`

Then toggle write interest with `EV_ENABLE` / `EV_DISABLE` instead of add/delete. Fewer
syscalls, and no risk of `ENOENT` from deleting a filter that isn't registered.

## 4.4 `ssize_t safe_recv(int fd, char* buf, size_t n)` / `safe_send`

Thin wrappers that retry on `EINTR` and otherwise return -1 with `errno` intact.

---

# 5. `src/server.cpp`

## 5.1 Connection state

```cpp
enum class Role { Unknown, Trader, MarketData };

struct Conn {
    int  fd;
    int  owner_id;
    Role role = Role::Unknown;
    std::string username;
    bool subs[2] = {false, false};
    LineBuffer in;
    OutBuffer  out;
    bool write_enabled = false;
    bool closing = false;       // drain `out`, then close (QUIT)
};
```

Globals (or members of a `Server` class — either is fine, a class is tidier):

```cpp
std::unordered_map<int, Conn>         conns_;      // fd -> Conn
std::unordered_map<int, int>          owner_fd_;   // owner_id -> fd (connected only)
std::unordered_set<std::string>       usernames_;
Exchange                              exch_;
int next_owner_ = 1;
bool verbose_ = false;                             // -v : log every recv size
```

`owner_fd_` is how you answer "is the originating trader still connected?" when a trade
fires. Erase from it on close; that alone gives you the §2.6 behaviour for free.

## 5.2 `void on_accept(int kq, int lfd)`

```
for (;;) {
    sockaddr_in peer{}; socklen_t plen = sizeof peer;
    int cfd = accept(lfd, (sockaddr*)&peer, &plen);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // drained
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EMFILE || errno == ENFILE) { log; break; }
        break;
    }
    set_nonblocking(cfd);
    Conn c; c.fd = cfd; c.owner_id = next_owner_++;
    conns_.emplace(cfd, std::move(c));
    kq_change(kq, cfd, EVFILT_READ,  EV_ADD | EV_ENABLE);
    kq_change(kq, cfd, EVFILT_WRITE, EV_ADD | EV_DISABLE);
}
```

The `for(;;)` matters: `kqueue` in level-triggered mode would re-fire, but accepting in a
loop is correct under both modes and handles a burst of connects in one wakeup.

## 5.3 `void on_readable(int kq, int fd)`

```
char tmp[4096];
for (;;) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n > 0) {
        if (verbose_) fprintf(stderr, "[recv] fd=%d n=%zd data=%.*s\n", fd, n, (int)n, tmp);
        c.in.append(tmp, (size_t)n);
        std::string line;
        for (;;) {
            LineStatus st = c.in.next_line(line);
            if (st == NeedMore) break;
            if (st == TooLong)  { queue_out(kq,c,msg_error("line too long")); c.closing=true; return; }
            handle_line(kq, c, line);
            if (c.closing) return;
        }
        continue;                       // try another recv
    }
    if (n == 0) { close_conn(kq, fd); return; }          // orderly FIN from peer
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return; // nothing more right now
    if (errno == ECONNRESET) { close_conn(kq, fd); return; }  // RST — Experiment 6
    close_conn(kq, fd); return;
}
```

That `verbose_` line is not optional decoration — **it is the deliverable for
Experiment 3.** Add a `-v` flag to `main` and the screenshot writes itself: you'll see
`n=11 data=BUY JNST 10` then `n=6 data=0 238\n`.

Distinguish `n == 0` (peer sent FIN, orderly) from `ECONNRESET` (peer sent RST, abrupt) in
your logs with different words. Experiment 6 asks you to tell those apart and a log line
saying exactly which one happened is the cleanest possible evidence.

## 5.4 `void queue_out(int kq, Conn& c, const std::string& msg)`

```
c.out.push(msg);
try_flush(kq, c);
```

Every single server→client message goes through this function. No bare `send()` anywhere
else in the file.

## 5.5 `void try_flush(int kq, Conn& c)`

```
while (!c.out.empty()) {
    ssize_t n = send(c.fd, c.out.ptr(), c.out.len(), 0);
    if (n > 0) { c.out.consume((size_t)n); continue; }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!c.write_enabled) { kq_change(kq,c.fd,EVFILT_WRITE,EV_ENABLE); c.write_enabled=true; }
        return;                              // socket send buffer full — this IS backpressure
    }
    close_conn(kq, c.fd); return;            // EPIPE / ECONNRESET
}
if (c.write_enabled) { kq_change(kq,c.fd,EVFILT_WRITE,EV_DISABLE); c.write_enabled=false; }
if (c.closing) close_conn(kq, c.fd);
```

Read that `EAGAIN` branch carefully — it's the single most important block in the whole
assignment. If you instead used a blocking socket, `send()` would sleep here and your
entire server would freeze because one market-data client stopped reading. Experiment 7
exists to make you observe exactly that. With this code, the slow client's `out` grows and
its `Send-Q` fills while everyone else is served normally, and you can show it with
`netstat -an`.

## 5.6 `void on_writable(int kq, int fd)`

Just `try_flush(kq, conns_.at(fd));`. The filter disables itself inside `try_flush` once
the buffer drains.

## 5.7 `void handle_line(int kq, Conn& c, const std::string& line)`

```
auto tk = split_ws(line);
if (tk.empty()) return;                 // ignore blank lines
const std::string& v = tk[0];

if (v == "LOGIN")            return cmd_login(kq, c, tk);
if (v == "BUY" || v=="SELL") return cmd_order(kq, c, tk, v=="BUY"?Side::Buy:Side::Sell);
if (v == "CANCEL")           return cmd_cancel(kq, c, tk);
if (v == "SUBSCRIBE")        return cmd_subscribe(kq, c, tk, true);
if (v == "UNSUBSCRIBE")      return cmd_subscribe(kq, c, tk, false);
if (v == "QUIT")             return cmd_quit(kq, c);
queue_out(kq, c, msg_error("unknown command"));
```

Role is decided by the first successful command: `LOGIN` promotes `Unknown → Trader`,
`SUBSCRIBE`/`UNSUBSCRIBE` promotes `Unknown → MarketData`. After that, each handler
rejects the wrong role with `ERROR not permitted for this client type` and **does not
disconnect** — the handout only requires that the command be refused.

### `cmd_login`
- `tk.size() != 2` → `ERROR bad arguments`.
- `c.role == MarketData` → not permitted.
- already logged in → `ERROR already logged in`.
- `usernames_.count(tk[1])` → `ERROR duplicate username`.
- Otherwise: `c.role = Trader; c.username = tk[1]; usernames_.insert(...); owner_fd_[c.owner_id]=c.fd;` → `OK`.

### `cmd_order(kq, c, tk, side)`
- `tk.size() != 4` → `ERROR bad arguments`.
- `c.role != Trader` → not permitted (covers both MD clients and pre-LOGIN clients; for
  the latter `ERROR not logged in` reads better — branch on `role == Unknown`).
- `inst = instrument_index(tk[1])`, `< 0` → `ERROR unknown instrument`.
- `parse_u32(tk[2],1,MAX,qty)` fail → `ERROR bad quantity`.
- `parse_u32(tk[3],1,MAX,price)` fail → `ERROR bad price`.
- **Only now** touch the exchange:
```
std::vector<Trade> trades;
uint32_t id = exch_.submit(side, inst, qty, price, c.owner_id, trades);
queue_out(kq, c, msg_order_accepted(id));
for (const Trade& t : trades) publish(kq, t);
```
Validation failure must not consume an order id — the handout says so explicitly
("must not accept or execute the order"), and a grader can detect a skipped id.

### `cmd_cancel`
`tk.size()!=2` → bad arguments; role check; `parse_u32(tk[1],0,kMaxOrderId,id)` fail →
`ERROR bad order id`; then `exch_.cancel(id, c.owner_id, err)` → `ORDER_CANCELLED <id>`
or `ERROR <err>`.

### `cmd_subscribe(kq, c, tk, bool on)`
`tk.size()!=2` → bad arguments; `c.role == Trader` → not permitted; promote `Unknown → MarketData`;
`inst` lookup; `c.subs[inst] = on;` → `OK`. Subscribing twice is idempotent, still `OK`.

### `cmd_quit`
`c.closing = true; try_flush(kq, c);` — flush anything pending, then close. Inside
`close_conn`, call `shutdown(fd, SHUT_WR)` before `close(fd)`; §4.1 requires you to use
`shutdown` somewhere and this is the natural place. It also gives Experiment 2 a clean
FIN to photograph.

## 5.8 `void publish(int kq, const Trade& t)`

```
auto bit = owner_fd_.find(t.buyer_owner);
if (bit != owner_fd_.end())
    queue_out(kq, conns_.at(bit->second), msg_bought(t.inst, t.qty, t.price));

auto sit = owner_fd_.find(t.seller_owner);
if (sit != owner_fd_.end())
    queue_out(kq, conns_.at(sit->second), msg_sold(t.inst, t.qty, t.price));

for (auto& [fd, c] : conns_)
    if (c.role == Role::MarketData && c.subs[t.inst] && !c.closing)
        queue_out(kq, c, msg_trade(t.inst, t.qty, t.price));
```

The `find` misses are the disconnected-trader case: trade proceeds, `TRADE` still goes out,
`BOUGHT`/`SOLD` silently skipped. Exactly §2.6.

**Iterator invalidation warning:** `queue_out` → `try_flush` → `close_conn` can erase from
`conns_` *while you are iterating it*. Fix: have `close_conn` push the fd onto a
`std::vector<int> dead_` instead of erasing, and reap `dead_` once per event-loop
iteration, after all events are processed. This is the bug most likely to give you a
mysterious crash on day 3.

## 5.9 `void close_conn(int kq, int fd)`

```
auto it = conns_.find(fd);  if (it == conns_.end()) return;
Conn& c = it->second;
if (!c.username.empty()) usernames_.erase(c.username);
owner_fd_.erase(c.owner_id);
shutdown(fd, SHUT_WR);       // best-effort; ignore ENOTCONN
close(fd);                   // closing an fd removes its kevents automatically
dead_.push_back(fd);         // erase from conns_ later, not here
```

Note for the viva: `close()` deregisters all kevents for that descriptor, so an explicit
`EV_DELETE` is unnecessary. Knowing that is a good answer; doing an explicit delete anyway
is also fine and arguably clearer.

Orders stay in `exch_` untouched. That's the point.

## 5.10 `int main(int argc, char** argv)`

```
const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
uint16_t port  = (argc > 2) ? (uint16_t)atoi(argv[2]) : 5000;
// also scan argv for "-v" -> verbose_ = true
signal(SIGPIPE, SIG_IGN);            // MUST. otherwise writing to a dead peer kills you
setvbuf(stderr, nullptr, _IONBF, 0);

int lfd = make_listen_socket(ip, port);
int kq  = kqueue();
kq_change(kq, lfd, EVFILT_READ, EV_ADD | EV_ENABLE);

struct kevent evs[64];
for (;;) {
    int n = kevent(kq, nullptr, 0, evs, 64, nullptr);   // nullptr timeout = block
    if (n < 0) { if (errno == EINTR) continue; perror("kevent"); break; }
    for (int i = 0; i < n; ++i) {
        int fd = (int)evs[i].ident;
        if (evs[i].flags & EV_EOF) { /* peer closed; still drain read below */ }
        if (fd == lfd)                            on_accept(kq, lfd);
        else if (evs[i].filter == EVFILT_READ)    on_readable(kq, fd);
        else if (evs[i].filter == EVFILT_WRITE)   on_writable(kq, fd);
    }
    for (int fd : dead_) conns_.erase(fd);
    dead_.clear();
}
```

`kevent` with a null timeout blocks. That is the correct answer to Experiment 4: an idle
client costs you nothing because you are asleep in `kevent`, not in `recv()` on that
client's socket. `procstat -k <pid>` will show the stack parked in `kqueue`/`kevent`, and
that screenshot is the deliverable.

Accept `argv[1]`/`argv[2]` as host and port because `experiment.py` will invoke
`./server/run-server 127.0.0.1 5000`. Read the actual `experiment.py` before finalising —
it defines the arguments.

---

# 6. `src/client_common.hpp`

Clients may use **blocking** sockets plus `select()`. Simpler, and completely legal. The
one thing they must do is multiplex stdin and the socket, otherwise a trader sitting in
`getline()` can never print an asynchronous `BOUGHT`.

## 6.1 `int connect_to(const char* host, const char* port)`

`socket(AF_INET, SOCK_STREAM, 0)`, fill `sockaddr_in` with `inet_pton` + `htons(atoi(port))`,
`connect()`. Exit with a message on failure.

## 6.2 `bool send_all(int fd, const std::string& s)`

```
size_t off = 0;
while (off < s.size()) {
    ssize_t n = send(fd, s.data()+off, s.size()-off, 0);
    if (n > 0) { off += n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return false;
}
return true;
```
Partial writes apply to clients too. Don't assume one `send` writes everything.

## 6.3 `void client_loop(int fd)`

```
setvbuf(stdout, nullptr, _IONBF, 0);        // CRITICAL — see note below
LineBuffer in;
bool stdin_open = true;
for (;;) {
    fd_set rs; FD_ZERO(&rs);
    FD_SET(fd, &rs);
    if (stdin_open) FD_SET(STDIN_FILENO, &rs);
    int mx = std::max(fd, STDIN_FILENO);
    if (select(mx+1, &rs, nullptr, nullptr, nullptr) < 0) { if (errno==EINTR) continue; break; }

    if (stdin_open && FD_ISSET(STDIN_FILENO, &rs)) {
        std::string line;
        if (!std::getline(std::cin, line)) {    // EOF / Ctrl-D
            send_all(fd, "QUIT\n");
            shutdown(fd, SHUT_WR);              // half-close: FIN, keep reading replies
            stdin_open = false;
        } else {
            send_all(fd, line + "\n");
        }
    }

    if (FD_ISSET(fd, &rs)) {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n == 0) { fprintf(stderr, "server closed connection\n"); break; }
        if (n < 0)  { if (errno==EINTR) continue; perror("recv"); break; }
        in.append(buf, (size_t)n);
        std::string msg;
        while (in.next_line(msg) == LineStatus::Ok) printf("%s\n", msg.c_str());
    }
}
close(fd);
```

**The unbuffered-stdout line is not cosmetic.** `experiment.py` runs your clients with
their stdout attached to a pipe, and C++ switches to full 4 KB buffering on a pipe. Your
output sits in userspace and the experiment sees nothing. Either `setvbuf(..., _IONBF, 0)`
or `fflush(stdout)` after each line. Several assignments die here every year.

The `shutdown(fd, SHUT_WR)` on EOF gives you a half-close to demonstrate in Experiment 2:
your side goes to `FIN_WAIT_2`, the server's to `CLOSE_WAIT`, and you can screenshot both
in `netstat -an -p tcp`.

---

# 7. `src/trader.cpp`

```cpp
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: trader <host> <port> <username>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int fd = connect_to(argv[1], argv[2]);
    send_all(fd, std::string("LOGIN ") + argv[3] + "\n");
    client_loop(fd);
}
```

Then the user types `BUY JNST 100 238` etc. on stdin.

# 8. `src/market_data.cpp`

Same shape; loop over `argv[3..]` so `run-market-data 127.0.0.1 5000 JNST IMCT` works, and
send a `SUBSCRIBE <inst>\n` for each. Accepting multiple instruments costs three lines and
covers you if the experiment script passes two.

---

# 9. `Makefile`

```make
CXX      := c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic
BIN      := exchange_server trader_client market_data_client

all: $(BIN)

exchange_server: src/server.cpp src/book.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

trader_client: src/trader.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

market_data_client: src/market_data.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(BIN)

.PHONY: all clean
```

FreeBSD's `make` is BSD make; the above is portable between BSD and GNU make. No
`-lpthread` needed — you're single-threaded. `kqueue` is in libc.

## Launchers

`server/run-server`:
```sh
#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
[ -x ./exchange_server ] || make >/dev/null 2>&1
exec ./exchange_server "$@"
```

`client/run-trader` and `client/run-market-data` mirror it with the other binaries. All
three: `chmod +x`. Verify the executable bit survives zipping — `unzip -l` won't show it,
so test with `unzip` into a fresh directory and run `ls -l`.

---

# 10. Build and test order

Work in this sequence; each step is verifiable before you write the next.

**Step 1.** `framing.hpp` + `protocol.hpp` + a throwaway `test_main.cpp` that feeds
`LineBuffer` bytes one at a time and prints extracted lines. Confirm splitting works.
Delete the test file afterwards or keep it out of `src/`.

**Step 2.** `book.cpp` + a throwaway main: submit `BUY JNST 100 238`, then
`SELL JNST 60 238`, assert one trade of 60 and 40 remaining. Then submit
`SELL JNST 60 239` and assert no trade (price must match exactly).

**Step 3.** Server that accepts connections and echoes back `OK` for everything. Verify
with `nc 127.0.0.1 5000`. Check `sockstat -4 -l | grep 5000` shows the listener — that's
half of Experiment 1 already done.

**Step 4.** Wire in the real dispatch and `Exchange`. Test with two `nc` sessions:

```sh
# terminal A
printf 'LOGIN alice\nBUY JNST 100 238\n' | nc 127.0.0.1 5000
# terminal B
printf 'LOGIN bob\nSELL JNST 60 238\n' | nc 127.0.0.1 5000
# terminal C
printf 'SUBSCRIBE JNST\n' | nc 127.0.0.1 5000
```
C must print `OK` then `TRADE JNST 60 238`.

**Step 5.** The framing proof, with `-v` on the server:
```sh
(printf 'BUY JNST 10'; sleep 1; printf '0 238\n'; sleep 2) | nc 127.0.0.1 5000
```
Two `[recv]` lines, one accepted order. Screenshot it — Experiment 3 is done.

**Step 6.** Real clients, then the checklist below.

## Correctness checklist

- Two traders match; partial fill leaves the correct remainder.
- Same price only: `BUY … 238` and `SELL … 239` must not trade.
- `CANCEL` on a resting order → `ORDER_CANCELLED`; on an unknown id → `ERROR`;
  on someone else's order → `ERROR`.
- `BUY JNST 0 238`, `BUY JNST -5 238`, `BUY JNST 2.5 238`, `BUY JNST 2147483648 238`,
  `BUY XXXX 1 1`, `BUY JNST 1` → all `ERROR`, and the next valid order's id must show that
  none of them consumed an id.
- MD client sending `BUY` → `ERROR`; trader sending `SUBSCRIBE` → `ERROR`.
- `LOGIN alice` twice from two connections → second gets `ERROR`.
- Trader submits, disconnects, another trader matches: `TRADE` fires, no crash.
- 10+ simultaneous clients (2 traders, 4+ MD) all served.
- Kill a client with `kill -9`: server logs it and keeps serving everyone else.

---

# 11. Mapping the code to the experiments

| Exp | What in your code produces the evidence |
|---|---|
| 1 | Listening fd vs connected fds — `sockstat -4`, `procstat -f <pid>` |
| 2 | `shutdown(SHUT_WR)` in `cmd_quit` / client EOF gives a clean FIN sequence |
| 3 | The `-v` `[recv]` log in `on_readable` |
| 4 | `kevent(..., nullptr)` blocking — `procstat -k <pid>` shows the stack in kevent |
| 6 | The `ECONNRESET` branch in `on_readable`, vs the `n == 0` branch |
| 7 | The `EAGAIN` branch in `try_flush` + growing `Send-Q` in `netstat -an` |
| 8 | `EV_EOF` / `n == 0` on the killed client while others keep running |

For Experiment 4 you can make the answer much stronger by adding a `--blocking` flag that
uses one blocking `accept`/`recv` per client in a loop, showing the server stuck in `recv`
on the idle client while client 2 waits. It's ~40 lines and it turns a description into a
demonstration. Only do it once everything else works.

---

# 12. README.md contents

- Language: C++17, compiled with `c++` (clang) on FreeBSD 14.4.
- Build: `make` from the submission root.
- Run server: `./server/run-server 127.0.0.1 5000` (add `-v` for per-recv logging).
- Run trader: `./client/run-trader 127.0.0.1 5000 alice`, then type protocol commands.
- Run MD client: `./client/run-market-data 127.0.0.1 5000 JNST`.
- Concurrency: single-threaded non-blocking `kqueue` event loop.
- Documented decisions: CANCEL restricted to the submitting trader; leading zeros accepted
  in numeric fields; per-connection output buffer capped at 8 MB; CRLF tolerated.