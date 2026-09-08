# Assignment 2 — The Socket Exchange

## Language and toolchain
C++17, compiled with `c++` (clang) on FreeBSD 14.4-RELEASE. No third-party
libraries; only the POSIX socket API, `kqueue`, and `select` (clients).
No threads, so no `-lpthread`.

## Build
    make

Produces `exchange_server`, `trader_client`, `market_data_client` in the
submission root. The launcher scripts run `make` automatically if a binary is
missing.

## Run
    ./server/run-server 127.0.0.1 5000          # add -v to log every recv()
    ./client/run-trader 127.0.0.1 5000 alice
    ./client/run-market-data 127.0.0.1 5000 JNST        # accepts several instruments

The trader reads protocol commands from stdin (`BUY JNST 100 238`,
`SELL IMCT 25 517`, `CANCEL 42`, `QUIT`). Ctrl-D sends `QUIT` and half-closes
the socket. The market-data client subscribes to each instrument named on the
command line and then prints `TRADE` updates as they arrive.

## Concurrency / I/O design
Single-threaded, non-blocking sockets, one `kqueue` event loop. Every accepted
descriptor is registered twice at accept time — `EVFILT_READ` enabled and
`EVFILT_WRITE` disabled — and write interest is later toggled with
`EV_ENABLE`/`EV_DISABLE` rather than add/delete, which avoids `ENOENT` and
halves the syscalls. `kevent()` is called with a NULL timeout, so the server
sleeps in the kernel whenever nothing is ready: an idle client costs nothing
and blocks nobody.

Each connection owns a `LineBuffer` (inbound framing) and an `OutBuffer`
(outbound queue with a read offset). All server→client bytes go through
`queue_out()` → `try_flush()`. When `send()` returns `EAGAIN` the remainder
stays queued, `EVFILT_WRITE` is enabled for that descriptor, and the loop
returns to serving everyone else — a slow reader applies backpressure to its
own connection only.

Clients use blocking sockets plus `select()` to multiplex stdin and the socket,
so an asynchronous `BOUGHT`/`SOLD` can be printed while the user is idle at the
prompt.

## Documented decisions
- **CANCEL is restricted to the submitting trader.** A cancel for another
  trader's order returns `ERROR not your order`.
- **Leading zeros are accepted** in numeric fields (`0100` == 100); they are
  unambiguous decimal. Decimals, signs, whitespace, and out-of-range values are
  rejected, and a rejected order never consumes an order id.
- **Per-connection output buffer capped at 8 MB.** A market-data client that
  never reads is disconnected rather than allowed to grow the server heap
  without bound.
- **CRLF is tolerated**: a trailing `\r` is stripped, so `nc` and `telnet` work.
- **Role is fixed by the first successful command**: `LOGIN` promotes a
  connection to Trader, `SUBSCRIBE`/`UNSUBSCRIBE` to Market-Data. A command
  wrong for the role is refused with
  `ERROR not permitted for this client type`; the connection is not dropped.
- **Time priority** is FIFO within a price level; matching requires exactly
  equal prices.
- **Disconnection does not cancel resting orders** (§2.6). Orders carry a
  logical `owner_id`, never a file descriptor, so they survive their client. If
  such an order later executes, subscribed market-data clients still receive
  `TRADE` and the `BOUGHT`/`SOLD` for the absent trader is silently skipped.
- `SIGPIPE` is ignored; writing to a departed peer surfaces as `EPIPE`.
- `SO_REUSEADDR` is set on the listening socket so the server can be restarted
  immediately while old connections linger in `TIME_WAIT`.

## Files
    src/framing.hpp        LineBuffer, OutBuffer (no socket calls)
    src/protocol.hpp       tokenising, strict validation, message builders
    src/book.hpp/.cpp      Order, Trade, Exchange matching engine
    src/netutil.hpp        set_nonblocking, make_listen_socket, kq_change
    src/server.cpp         Conn, dispatch, kqueue event loop
    src/client_common.hpp  connect_to, send_all, select() client loop
    src/trader.cpp         Trader Client entry point
    src/market_data.cpp    Market-Data Client entry point
