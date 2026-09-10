# Assignment 2 — The Socket Exchange

**Team:** 2024CS10573, 2024CS10836

A TCP-based trading system: one Exchange Server, plus Trader and Market-Data clients, all
communicating over the loopback interface using the raw POSIX socket API.

---

## 1. Language and toolchain

| Item | Value |
|---|---|
| Language | C++17 |
| Compiler | `c++` (clang), the FreeBSD base system compiler |
| Flags | `-std=c++17 -O2 -Wall -Wextra -Wpedantic` |
| Target platform | FreeBSD 14.4-RELEASE (developed and tested on ARM64 under UTM) |
| Server concurrency / I/O | single-threaded, non-blocking sockets, `kqueue` event loop |
| Client concurrency / I/O | blocking sockets, `select()` multiplexing stdin and the socket |
| External dependencies | **none** — POSIX sockets, `kqueue` and `select` only |

No threads are used, so no `-lpthread` is required. No third-party networking library, event loop,
or framework is used anywhere in the implementation.

---

## 2. Build

From the root of the submission directory:

```sh
make
```

This produces three executables in the submission root:

| Binary | Built from |
|---|---|
| `exchange_server` | `src/server.cpp`, `src/book.cpp` |
| `trader_client` | `src/trader.cpp` |
| `market_data_client` | `src/market_data.cpp` |

To remove them:

```sh
make clean
```

### 2.1 Launcher permissions

The three launcher scripts must be executable. Some archive tools do not preserve the executable
bit, so if a launcher reports `Permission denied`, run:

```sh
chmod +x server/run-server client/run-trader client/run-market-data
```

The launchers invoke `make` automatically if the corresponding binary is missing, and use `exec` so
that the launcher process is replaced by the actual program.

---

## 3. Running the Exchange Server

```sh
./server/run-server <ip> <port>
```

Example:

```sh
./server/run-server 127.0.0.1 5000
```

Both arguments are optional and default to `127.0.0.1` and `5000`. The server logs connection
events to **stderr** and runs until interrupted with Ctrl-C.

---

## 4. Running the clients

### 4.1 Trader Client

```sh
./client/run-trader <ip> <port> <username>
```

Example:

```sh
./client/run-trader 127.0.0.1 5000 alice
```

The client sends `LOGIN <username>` automatically on connect, then reads protocol commands from
stdin, one per line:

```
BUY JNST 100 238
SELL IMCT 25 517
CANCEL 42
QUIT
```

Server responses and asynchronous execution notifications (`BOUGHT` / `SOLD`) are printed to stdout
as they arrive, including while the user is idle at the prompt. Pressing Ctrl-D sends `QUIT` and
half-closes the socket, after which the client continues reading until the server closes.

### 4.2 Market-Data Client

```sh
./client/run-market-data <ip> <port> <instrument> [<instrument> ...]
```

Examples:

```sh
./client/run-market-data 127.0.0.1 5000 JNST
./client/run-market-data 127.0.0.1 5000 JNST IMCT
```

The client sends `SUBSCRIBE <instrument>` for every instrument named on the command line, then
prints `TRADE` updates as they arrive. Further `SUBSCRIBE` / `UNSUBSCRIBE` / `QUIT` commands may be
typed on stdin.

Supported instruments: `JNST`, `IMCT`.

---

## 5. Configuration

**No configuration is required.** There are no config files, environment variables, or external
services that must be set up. All state is held in memory, and the listening address and port are
passed on the command line.

### 5.1 Optional diagnostic logging

Setting `SX_VERBOSE=1` in the environment, or passing `-v` on the command line, enables two extra
stderr log lines used as evidence in the experiment report:

```sh
SX_VERBOSE=1 ./server/run-server 127.0.0.1 5000
./server/run-server 127.0.0.1 5000 -v
```

| Log line | Meaning |
|---|---|
| `[recv] fd=N n=B data=…` | every `recv()` with its byte count and payload — demonstrates message framing |
| `[flush] fd=N send() EAGAIN, B bytes still queued` | the moment a non-blocking write is refused — demonstrates backpressure |

The environment-variable form exists because the provided `experiment.py` launches the server with a
fixed argument list; the environment is inherited through it. **The server is silent by default**
and behaves identically with the flag unset.

### 5.2 Quick manual check

```sh
./server/run-server 127.0.0.1 5000 &
printf 'SUBSCRIBE JNST\n'                  | nc 127.0.0.1 5000 &   # market data
printf 'LOGIN alice\nBUY JNST 100 238\n'   | nc 127.0.0.1 5000 &   # buyer
printf 'LOGIN bob\nSELL JNST 60 238\n'     | nc 127.0.0.1 5000     # seller
# the market-data session prints: OK, then TRADE JNST 60 238
```

---

## 6. Directory layout

```
.
├── server/run-server              launcher for the Exchange Server
├── client/run-trader              launcher for the Trader Client
├── client/run-market-data         launcher for the Market-Data Client
├── src/
│   ├── framing.hpp                LineBuffer (inbound framing), OutBuffer (outbound queue)
│   ├── protocol.hpp               tokenising, strict numeric validation, message builders
│   ├── book.hpp / book.cpp        Order, Trade, Exchange matching engine
│   ├── netutil.hpp                set_nonblocking, make_listen_socket, kq_change
│   ├── server.cpp                 Conn state, protocol dispatch, kqueue event loop
│   ├── client_common.hpp          connect_to, send_all, select() client loop
│   ├── trader.cpp                 Trader Client entry point
│   ├── market_data.cpp            Market-Data Client entry point
│   └── tools/                     bonus-only tools (see §8)
├── Makefile
├── README.md
└── report.pdf
```

---

## 7. Implementation notes and documented decisions

These are choices where the specification permits more than one reading. They are described in full
in `report.pdf`; they are listed here so they are not read as oversights.

- **Concurrency:** a single-threaded `kqueue` event loop with non-blocking sockets. Each accepted
  descriptor is registered with `EVFILT_READ` enabled and `EVFILT_WRITE` disabled, and write
  interest is toggled with `EV_ENABLE` / `EV_DISABLE` rather than add/delete.
- **Framing:** each connection owns a `LineBuffer`; messages are extracted by scanning for `\n` in a
  loop, so a message split across several `recv()` calls and several messages arriving in one
  `recv()` are both handled. A trailing `\r` is stripped, so `nc` and `telnet` work unmodified.
- **Backpressure:** on `EAGAIN` the undelivered bytes remain in that connection's own `OutBuffer`
  and `EVFILT_WRITE` is enabled for that descriptor alone. The buffer is capped at **8 MB** per
  connection; a client exceeding it is disconnected, so a subscriber that never reads cannot grow
  the server heap without bound.
- **Roles:** the protocol has no handshake announcing the client type, so the role is fixed by the
  first successful command — `LOGIN` promotes a connection to Trader, `SUBSCRIBE` / `UNSUBSCRIBE`
  to Market-Data. A command not permitted for the role is refused with
  `ERROR not permitted for this client type`; the connection is **not** dropped.
- **`CANCEL` is restricted to the submitting trader.** Cancelling another trader's order returns
  `ERROR not your order`.
- **Leading zeros are accepted** in numeric fields (`0100` == 100) as unambiguous decimal. Decimals,
  signs, whitespace, and out-of-range values are rejected, and a rejected order never consumes an
  order ID.
- **Matching** requires exactly equal prices; time priority is FIFO within a price level.
- **Disconnection does not cancel resting orders** (handout §2.6). Orders carry a logical
  `owner_id`, never a file descriptor, so they survive their client. If such an order later
  executes, subscribed Market-Data Clients still receive `TRADE`, and the `BOUGHT` / `SOLD` for the
  absent trader is silently skipped.
- `SIGPIPE` is ignored; `SO_REUSEADDR` is set on the listening socket; `shutdown(fd, SHUT_WR)` is
  issued before `close()` on teardown.
- Clients call `setvbuf(stdout, nullptr, _IONBF, 0)` so their output is not lost to block buffering
  when stdout is a pipe.

---

## 8. Bonus (§6.9) — connection scalability

The bonus tools are in `src/tools/` and are **not** built by `make`, since they are not part of the
graded implementation:

```sh
c++ -std=c++17 -O2 -Wall -o connflood       src/tools/connflood.cpp
c++ -std=c++17 -O2 -Wall -o threaded_server src/tools/threaded_server.cpp -lpthread
chmod +x src/tools/measure.sh
```

| File | Purpose |
|---|---|
| `src/tools/connflood.cpp` | client-generation program: creates and holds N simultaneous idle TCP connections, exchanging no application data |
| `src/tools/measure.sh` | captures one row of the resource-measurement table |
| `src/tools/threaded_server.cpp` | **comparison artifact only.** A minimal thread-per-connection server used to quantify the trade-off in the bonus analysis. It is **not** the submitted Exchange Server, which is single-threaded and uses `kqueue`. |

Running the ladder requires kernel tuning, documented in full in `report.pdf` §9.2. In outline:

```sh
# loopback aliases — required, since one (src ip, dst ip, dst port) triple has only
# ~64,500 ephemeral ports and so cannot reach 70,000 connections
ifconfig lo0 alias 127.0.0.2/32
ifconfig lo0 alias 127.0.0.3/32

ulimit -n 200000        # in both the server and generator shells
./server/run-server 127.0.0.1 5000 > /dev/null 2>&1
./connflood 127.0.0.1 5000 70000 --src 127.0.0.1,127.0.0.2,127.0.0.3 --report 2500 --batch 64
src/tools/measure.sh 70000
```

---

## 9. Reproducing the experiments

```sh
python3 experiment.py <N>                                  # experiments 1-8
SX_VERBOSE=1 python3 -u experiment.py 3 2>&1 | tee out.log # with per-recv logging
```

Experiment 7 additionally requires the socket buffers to be clamped, or the volume of market data
fits entirely within the default auto-tuned buffers and flow control never engages:

```sh
sysctl kern.ipc.maxsockbuf=16384
sysctl net.inet.tcp.recvspace=4096 net.inet.tcp.sendspace=4096
sysctl net.inet.tcp.recvbuf_auto=0 net.inet.tcp.sendbuf_auto=0
# restore the defaults afterwards, before running experiment 8
```

Between runs:

```sh
pkill exchange_server
ktrace -C
pgrep -f exchange_server        # must print nothing before the next experiment
```
