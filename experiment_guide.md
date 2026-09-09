# The Socket Exchange — Investigation Guide for Experiments 1–8
### FreeBSD 14.4-RELEASE · single-threaded non-blocking `kqueue` server · harness `experiment.py`

---

## 0. One-time preparation (do all of this before Experiment 1)

### 0.1 Facts extracted from `experiment.py` that govern every run

| Fact | Consequence for your investigation |
|---|---|
| Server is always launched as `./server/run-server 127.0.0.1 5000` | Port is fixed at **5000**; every filter below can hardcode it. |
| No extra argv is passed to the server | `-v` never arrives. Use the env-var patch in §0.3. |
| Launcher uses `exec` | The PID printed as `Started Exchange Server (PID N)` **is** `exchange_server`. Verify: `procstat -b N`. |
| Server stdout/stderr are inherited (no redirect in `Popen`) | Your `[recv]` / `[server]` log lines appear in Terminal 1, interleaved with harness output. Capture with `tee`. |
| `wait_for_server()` connects a probe and closes it with `SO_LINGER{1,0}` | **Every experiment begins with a spurious SYN/SYN-ACK/ACK + RST in `tcpdump`.** Say so in your report and exclude it; mistaking it for Exp 6's RST is the single most common error. |
| Harness prints each client's source port (`connected from 127.0.0.1:PORT`) | This is your ground truth for mapping ephemeral port → client identity → server fd. **Screenshot Terminal 1 every time.** |

### 0.2 Record the kernel baseline (include this table once in the report)

```sh
sysctl net.inet.tcp.msl \
       net.inet.tcp.finwait2_timeout \
       net.inet.tcp.fast_finwait2_recycle \
       net.inet.tcp.sendspace net.inet.tcp.recvspace \
       net.inet.tcp.sendbuf_auto net.inet.tcp.recvbuf_auto \
       net.inet.tcp.sendbuf_max  net.inet.tcp.recvbuf_max \
       net.inet.tcp.keepidle net.inet.tcp.keepintvl net.inet.tcp.always_keepalive \
       kern.ipc.maxsockbuf kern.maxfiles kern.maxfilesperproc
sysctl -a | grep -i timewait          # see the note in Experiment 2
```

Typical FreeBSD 14 defaults: `msl=30000` (ms) so TIME_WAIT ≈ 2×MSL ≈ 60 s; `sendspace=32768`,
`recvspace=65536`, both auto-tuned upward to `*_max`. Record *your* values — the numbers in
Experiment 7 only make sense against them.

### 0.3 Two small source patches (both are legitimate instrumentation, not cheating)

**(a) Verbose logging reachable from the harness.** In `src/server.cpp`, inside `main()`, just
after the argv scan:

```cpp
    if (const char* e = getenv("SX_VERBOSE"); e && *e && e[0] != '0') g_verbose = true;
```

(add `#include <cstdlib>` — already present). Now run any experiment as:

```sh
SX_VERBOSE=1 python3 experiment.py 3 2>&1 | tee /tmp/exp3.log
```

`Popen` inherits the environment, so the flag reaches the server through the launcher unchanged.
Alternative if you prefer no code change: temporarily edit `server/run-server` to
`exec ./exchange_server "$@" -v`.

**(b) Log the exact instant a non-blocking write refuses.** In `try_flush()`, inside the
`EAGAIN/EWOULDBLOCK` branch, before enabling the filter:

```cpp
                if (g_verbose)
                    std::fprintf(stderr,
                        "[flush] fd=%d send() EAGAIN, %zu bytes still queued, enabling EVFILT_WRITE\n",
                        c.fd, c.out.pending());
```

and in the drain path of `on_writable`, after `try_flush`, optionally log `pending()`. This one
line is the primary Experiment 7 deliverable: it timestamps the transition from "kernel accepted
my bytes" to "kernel socket buffer is full, application-level backpressure begins".

Rebuild: `make clean && make`.

### 0.4 Helper scripts (`~/tools/`, `chmod +x` all three)

`~/tools/poll-tcp.sh` — timestamped kernel state poller:

```sh
#!/bin/sh
# usage: poll-tcp.sh [interval] [logfile]
INT=${1:-0.2}; LOG=${2:-/tmp/tcpstates.log}
while :; do
  printf '=== %s ===\n' "$(date '+%H:%M:%S')"
  netstat -an -p tcp | awk 'NR<=2 || /\.5000[[:space:]]/ || /\.5000$/'
  sleep "$INT"
done | tee -a "$LOG"
```

`~/tools/poll-tcpx.sh` — extended socket-buffer / timer poller (Experiments 5, 7, 8):

```sh
#!/bin/sh
INT=${1:-0.5}; LOG=${2:-/tmp/tcpx.log}
while :; do
  printf '=== %s ===\n' "$(date '+%H:%M:%S')"
  netstat -an -p tcp -x | awk 'NR<=2 || /\.5000/'
  sleep "$INT"
done | tee -a "$LOG"
```

`~/tools/fdmap.sh` — fd ↔ 4-tuple table for the server:

```sh
#!/bin/sh
PID=$(pgrep -n exchange_server) || exit 1
printf 'server pid=%s\n' "$PID"
procstat -f "$PID" | egrep 'PID|TCP|kqueue'
echo '--- sockstat ---'
sockstat -4 -p 5000        # add -s for the TCP state column if your sockstat supports it
```

`sleep` accepts fractional seconds on FreeBSD. `date` (BSD) has no `%N`; sub-second resolution
comes from `tcpdump`, which is why every experiment below runs a capture in parallel — the pcap
is your authoritative timeline and the netstat poller is the kernel-state timeline beside it.

### 0.5 Standard terminal layout (identical for all 8 experiments)

| Terminal | Role | Started |
|---|---|---|
| **T1** | Harness + server stderr | last (after T2–T4 are already running) |
| **T2** | Kernel TCP state: `poll-tcp.sh` / `poll-tcpx.sh` | first |
| **T3** | Packet trace: `tcpdump` live print + a parallel `-w` capture | first |
| **T4** | Process/OS inspection: `sockstat`, `procstat -f/-k`, `ktrace`, `top` | on demand, once the server PID is known |

Two `tcpdump` instances can attach to `lo0` at once (BPF clones the device), so run both:

```sh
# T3a — human-readable, teed to a text log for the report
tcpdump -i lo0 -n -S -vv -tttt 'tcp port 5000' | tee /tmp/expN.txt
# T3b — archival pcap
tcpdump -i lo0 -n -s0 -B 4096 -w /tmp/expN.pcap 'tcp port 5000'
```

Flag rationale, worth one line in your report: `-n` no DNS/service lookup (so you see `5000`, not
`commplex-main`); `-S` absolute sequence numbers, required to reason about the FIN's sequence
consumption; `-tttt` absolute wall-clock timestamps so the pcap can be aligned with the netstat
poller and the server log; `-vv` exposes window, options and TCP flags; `-B` enlarges the BPF
buffer so Experiment 7's burst is not dropped (`tcpdump` prints "packets dropped by kernel" if it
was — check that line every time, and re-run if it is non-zero).

All of `tcpdump`, `ktrace` and `procstat -k` need root. Run everything as root or via `sudo`.

### 0.6 Between runs

```sh
ktrace -C                            # clear any leftover tracing
pgrep -f exchange_server             # must be empty before the next run
rm -f /tmp/exp*.txt /tmp/exp*.pcap /tmp/tcpstates.log /tmp/tcpx.log
```

---

# Experiment 1 — Listening and Connected Sockets

Harness behaviour: starts the server, opens **one** client connection, leaves it established and
idle until you press Ctrl-C. Nothing is ever sent on it.

### 1.1 Terminal setup & strategy

Because the connection is idle and persists indefinitely, this is a static-state experiment: the
strategy is to photograph the same two sockets through three independent kernel views —
`netstat` (the protocol control block table), `sockstat` (PCBs joined to processes), and
`procstat -f` (the per-process file-descriptor table) — and show they agree.

| Terminal | Command |
|---|---|
| T2 | `~/tools/poll-tcp.sh 1` |
| T3 | `tcpdump -i lo0 -n -S -tttt -vv 'tcp port 5000' \| tee /tmp/exp1.txt` |
| T1 | `python3 experiment.py 1 2>&1 \| tee /tmp/exp1.log` |
| T4 | inspection commands below |

### 1.2 Exact commands

T1 prints `Started Exchange Server (PID <N>)` and `Experiment client: connected from 127.0.0.1:<E>`.
Note both numbers. Then in T4:

```sh
PID=$(pgrep -n exchange_server); echo $PID          # cross-check against the harness output
procstat -b $PID                                    # confirms exec() replaced the shell: path ends in /exchange_server

# View 1 — protocol view
netstat -an -p tcp | egrep 'Proto|\.5000'

# View 2 — process↔socket view
sockstat -4 -p 5000
sockstat -4 -l -p 5000                              # listening sockets only
sockstat -4 -c -p 5000                              # connected sockets only

# View 3 — file-descriptor view
procstat -f $PID | egrep 'PID|TCP|kqueue'

# Extra: the kqueue descriptor itself, and the listen backlog
procstat -f $PID | grep kqueue
netstat -aLn -p tcp                                 # listen queue: qlen/incqlen/maxqlen for 127.0.0.1.5000
```

### 1.3 Observations to capture

Screenshot 1 (`netstat`) must show **three** rows for port 5000:

```
tcp4  0  0  127.0.0.1.5000     *.*                LISTEN
tcp4  0  0  127.0.0.1.5000     127.0.0.1.<E>      ESTABLISHED     <- server side
tcp4  0  0  127.0.0.1.<E>      127.0.0.1.5000     ESTABLISHED     <- client side (the harness)
```

The listener's foreign address is the wildcard `*.*`; both `Recv-Q` and `Send-Q` are 0 everywhere.

Screenshot 2 (`procstat -f $PID`) must show, for the same PID, distinct descriptors:

```
 PID COMM             FD T V FLAGS   REF OFFSET PRO NAME
 <N> exchange_server   3 s - rw----  ...          TCP 127.0.0.1:5000
 <N> exchange_server   4 k - rw----  ...          kqueue ...
 <N> exchange_server   5 s - rw----  ...          TCP 127.0.0.1:5000 127.0.0.1:<E>
```

fd 3 is the passive listener (no foreign endpoint), fd 5 the accepted connection (full 4-tuple),
fd 4 the `kqueue` — include that row, it is free evidence for your Implementation Decisions
section. (Your accepted fd may be 5 even though the readiness probe ran earlier: the probe's
descriptor was closed and its number recycled.)

Screenshot 3 (`sockstat -4 -l` vs `-c`) shows the same split from the process side.

Screenshot 4 (`netstat -aLn -p tcp`) shows `0/0/128` for `127.0.0.1.5000` — the listener owns a
queue, not a data stream.

### 1.4 Answer

The Exchange Server owns two structurally different TCP sockets. The **listening socket** (fd 3)
is bound to the local half of an address pair only — `127.0.0.1.5000` with a wildcard foreign
address — and carries no data: it holds the SYN/accept queues, and `netstat -aLn` shows its
backlog counters rather than `Recv-Q`/`Send-Q`. The **connected socket** (fd 5) returned by
`accept()` is identified by the complete 4-tuple `127.0.0.1:5000 ↔ 127.0.0.1:<E>` and is the only
one over which bytes can flow. The kernel demultiplexes an arriving segment by searching its inpcb
hash for an exact 4-tuple match first, falling back to the wildcard listener only for a segment
that matches no established connection (i.e. a SYN). This is why one bound port can serve
arbitrarily many simultaneous clients: each connection is distinguished by the peer's ephemeral
port, and each gets its own descriptor, its own socket buffers, and its own `Conn` object in the
server.

---

# Experiment 2 — Observing TCP Connection States

Harness behaviour: connect; **10 s idle**; `close_socket(client)` — a full `close()`, so a FIN is
sent and the client socket is fully closed; server stays up **15 s** more. You get one shot at the
transition, so all observers must already be running.

### 2.1 Terminal setup & strategy

The teardown on loopback completes in microseconds, so a 0.2 s poller may miss the intermediate
states. The strategy is therefore two-layered: `tcpdump` supplies the authoritative packet-level
timeline (which cannot be missed), and the poller supplies whichever kernel states persist long
enough to photograph — in practice `ESTABLISHED` and `TIME_WAIT`. States that are traversed
instantaneously are then proven from the packet trace plus a deliberate control experiment (§2.5).

| Terminal | Command |
|---|---|
| T2 | `~/tools/poll-tcp.sh 0.1 /tmp/exp2.states` |
| T3a | `tcpdump -i lo0 -n -S -vv -tttt 'tcp port 5000' \| tee /tmp/exp2.txt` |
| T3b | `tcpdump -i lo0 -n -s0 -w /tmp/exp2.pcap 'tcp port 5000'` |
| T1 | `python3 experiment.py 2 2>&1 \| tee /tmp/exp2.log` |
| T4 | snapshots at each phase |

### 2.2 Exact commands

```sh
# T4, during phase 1 (the 10 s idle window)
PID=$(pgrep -n exchange_server)
netstat -an -p tcp | egrep 'Proto|\.5000'
procstat -f $PID | grep TCP
sockstat -4 -p 5000

# T4, immediately after the harness prints "Phase 2: closing the client connection."
netstat -an -p tcp | egrep 'Proto|\.5000'          # run it two or three times in a row
procstat -f $PID | grep TCP                         # the connected fd is gone from the table

# after the harness exits, TIME_WAIT survives the process:
netstat -an -p tcp | grep TIME_WAIT
sysctl net.inet.tcp.msl                             # 30000 ms -> 2*MSL = 60 s
```

Replay the capture afterwards for the report screenshot:

```sh
tcpdump -r /tmp/exp2.pcap -n -S -tttt -vv
```

### 2.3 Observations to capture

**Establishment** — three segments, and note that the server-side socket appears in `netstat`
only after the handshake completes:

```
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [S],  seq X,       win 65535, ...
IP 127.0.0.1.5000 > 127.0.0.1.<E>: Flags [S.], seq Y, ack X+1, ...
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [.],  ack Y+1
```

**Steady state** — both endpoints `ESTABLISHED`, `Recv-Q = Send-Q = 0` on both.

**Teardown** — expect **three** segments, not four:

```
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [F.], seq X+1, ack Y+1      <- client close()
IP 127.0.0.1.5000 > 127.0.0.1.<E>: Flags [F.], seq Y+1, ack X+2      <- server ACK + FIN, coalesced
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [.],  ack Y+2
```

Explain the coalescing explicitly — it earns marks. The server's ACK of the client's FIN and the
server's own FIN travel in **one** segment because the event loop reacts within the same `kevent`
wakeup: `recv()` returns 0, `close_conn()` runs immediately, so by the time the delayed-ACK timer
would have fired there is already a FIN to piggyback the ACK onto. The four-way teardown of the
textbook becomes three segments whenever the peer application closes promptly.

**Kernel states** — from `/tmp/exp2.states`, you will reliably capture `ESTABLISHED` on both
sides, then `TIME_WAIT` on the client side (`127.0.0.1.<E> → 127.0.0.1.5000`) persisting for
~60 s, while the server side disappears entirely. Grep the log for the transient states and report
honestly what you caught:

```sh
egrep -n 'FIN_WAIT|CLOSE_WAIT|LAST_ACK|TIME_WAIT' /tmp/exp2.states | head -40
```

### 2.4 The TIME_WAIT / loopback sysctl question

Older FreeBSD carried `net.inet.tcp.nolocaltimewait`, which suppressed the TIME_WAIT state for
connections whose peer was local. That knob belonged to the compressed-TIME_WAIT (`tcptw`)
machinery, which was removed in the FreeBSD 14 TCP rework, so on 14.4 the sysctl is generally
**absent** and loopback TIME_WAIT is plainly visible. Do not assert either way from memory —
run and screenshot:

```sh
sysctl -a | grep -i timewait     ;  sysctl net.inet.tcp.nolocaltimewait   # may report "unknown oid"
```

Report what your kernel actually has. If the OID exists and is 1, set `sysctl
net.inet.tcp.nolocaltimewait=0`, re-run Experiment 2, and show TIME_WAIT appearing — that
comparison is worth including. If the OID does not exist, state that, cite `net.inet.tcp.msl=30000`
as the parameter that sets the ~60 s dwell, and show the TIME_WAIT entry ageing out.

### 2.5 Control experiment for CLOSE_WAIT (recommended)

With your server, `CLOSE_WAIT` lasts microseconds — the loop closes the moment `recv()` returns 0
— so it is essentially unphotographable, and claiming a screenshot of it would be dishonest.
Prove the state is traversed two ways:

1. **Syscall order**, which shows the application closing in direct response to EOF:
   ```sh
   ktrace -i -f /tmp/exp2.kt -p $PID -t c
   # ... let the harness close the client ...
   ktrace -C ; kdump -f /tmp/exp2.kt -T | egrep 'recvfrom|shutdown|close|kevent' | tail -20
   ```
   Expect `RET recvfrom 0` followed immediately by `CALL shutdown(...,SHUT_WR)` and `CALL close(...)`
   on the same descriptor.
2. **A deliberately lazy peer**, where the state is trivially visible:
   ```sh
   # T-A
   nc -l 127.0.0.1 5001
   # T-B
   python3 -c 'import socket,time; s=socket.create_connection(("127.0.0.1",5001)); s.shutdown(socket.SHUT_WR); time.sleep(30)'
   # T-C
   netstat -an -p tcp | grep 5001      # nc side: CLOSE_WAIT ; python side: FIN_WAIT_2
   ```

### 2.6 Answer

The connection passes through `SYN_SENT`/`SYN_RCVD` during the three-way handshake and settles in
`ESTABLISHED` on both endpoints. When the client calls `close()`, its stack sends FIN and moves
`ESTABLISHED → FIN_WAIT_1`; the server's stack moves `ESTABLISHED → CLOSE_WAIT` and delivers EOF
to the application as `recv() == 0`. Because the event loop closes the descriptor within the same
wakeup, the server's ACK and its own FIN are emitted as a single segment, so the server transits
`CLOSE_WAIT → LAST_ACK → CLOSED` in microseconds and the client transits
`FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT` on receipt of that segment. The client-side endpoint then
remains in `TIME_WAIT` for 2×MSL (≈60 s with `net.inet.tcp.msl=30000`), which is why it is still in
`netstat` after the harness process itself has exited: TIME_WAIT is owned by the kernel's PCB, not
by the process. The events causing the transitions are, in order: connect/SYN, SYN-ACK, ACK,
application `close()` → FIN, peer application `close()` → FIN+ACK, final ACK, then the 2×MSL timer.

---

# Experiment 3 — TCP as a Byte Stream

Harness behaviour: sends `LOGIN experiment_trader\n` as **four** separate `send()` calls —
`b"LOGIN "` (6 B), `b"experiment"` (10 B), `b"_trader"` (7 B), `b"\n"` (1 B) — with 0.2 s between
them, then idles until Ctrl-C.

### 3.1 Terminal setup & strategy

The proof has to be made on both sides of the boundary simultaneously: four discrete TCP segments
on the wire, and four discrete `recv()` returns in the application, followed by exactly **one**
application-level message being acted upon. The 0.2 s spacing guarantees the segments cannot
coalesce, so this is fully deterministic.

| Terminal | Command |
|---|---|
| T3a | `tcpdump -i lo0 -n -S -vv -tttt 'tcp port 5000' \| tee /tmp/exp3.txt` |
| T2 | `~/tools/poll-tcp.sh 0.1` (watch `Recv-Q`) |
| T1 | `SX_VERBOSE=1 python3 experiment.py 3 2>&1 \| tee /tmp/exp3.log` |
| T4 | `ktrace` on the server (below) |

### 3.2 Exact commands

```sh
# T4, as soon as the server PID is printed and before the pieces are sent:
PID=$(pgrep -n exchange_server)
ktrace -i -f /tmp/exp3.kt -p $PID -t c
# ... let all four pieces go out ...
ktrace -C
kdump -f /tmp/exp3.kt -T | egrep 'kevent|recvfrom|sendto|write' | head -60

# afterwards
grep '\[recv\]' /tmp/exp3.log
tcpdump -r /tmp/exp3.pcap -n -S -tttt 2>/dev/null   # if you also ran the -w capture
```

### 3.3 Observations to capture

**Wire (T3a)** — four data segments, each with PSH set, 0.2 s apart, with lengths 6, 10, 7, 1 and
strictly increasing sequence numbers, each individually ACKed by the server:

```
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [P.], seq 1:7,   ack 1, length 6
IP 127.0.0.1.5000 > 127.0.0.1.<E>: Flags [.],  ack 7
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [P.], seq 7:17,  ack 1, length 10
...
IP 127.0.0.1.<E> > 127.0.0.1.5000: Flags [P.], seq 24:25, ack 1, length 1
IP 127.0.0.1.5000 > 127.0.0.1.<E>: Flags [P.], seq 1:4,   ack 25, length 3     <- "OK\n"
```

**Application (T1)** — four `[recv]` lines with exactly matching byte counts, and the `OK` only
after the fourth:

```
[recv] fd=5 n=6  data=LOGIN
[recv] fd=5 n=10 data=experiment
[recv] fd=5 n=7  data=_trader
[recv] fd=5 n=1  data=
```

**Syscalls (T4)** — `kdump` shows the one-to-one correspondence and, crucially, the `EAGAIN` that
ends each read burst:

```
CALL  kevent(...)          RET kevent 1
CALL  recvfrom(0x5,...)    RET recvfrom 6
CALL  recvfrom(0x5,...)    RET -1 errno 35 Resource temporarily unavailable
```

**Kernel queue (T2)** — `Recv-Q` is 0 in every snapshot. Point this out: the bytes are not waiting
in the socket buffer, they have already been copied into the server's `LineBuffer`, where they sit
as an *incomplete application message*. Buffering for framing happens in userspace, not in the
kernel.

### 3.4 Answer

The message is not received as a unit. Four writes produced four TCP segments and four `recv()`
returns of 6, 10, 7 and 1 bytes; the server only produced `ORDER`-level behaviour (the `OK`
response to `LOGIN`) after the fourth `recv()` delivered the `\n`. TCP guarantees an ordered,
reliable **byte stream** and nothing else: it preserves neither the sender's write boundaries nor
any notion of a message, and is free to split or coalesce data at any point. Application message
boundaries therefore exist only in the application, which is why the server accumulates bytes in a
per-connection `LineBuffer` that persists across `recv()` calls and extracts messages by scanning
for the `\n` delimiter in a loop. The same loop handles the converse case — several messages
arriving in one `recv()` — which is why the extraction is `while (next_line(...) == Ok)` rather
than a single call.

---

# Experiment 4 — One Client Should Not Stall the Others

Harness behaviour: Client 1 connects and sends `LOGIN blocked_client` (20 bytes, **no newline**),
then goes silent forever. Two seconds later Client 2 connects, sends `LOGIN active_client\n`, and
the harness times the round trip.

### 4.1 Terminal setup & strategy

The question is *where the server's thread is*. The strategy is to sample the server's kernel
stack repeatedly across the whole run and show it is always parked in `kevent` — never in
`recvfrom` on Client 1's descriptor — and to corroborate that with a syscall trace showing that
Client 1's fd is simply not among the ready descriptors, plus the harness's own latency
measurement.

| Terminal | Command |
|---|---|
| T3 | `tcpdump -i lo0 -n -S -tttt -vv 'tcp port 5000' \| tee /tmp/exp4.txt` |
| T2 | `~/tools/poll-tcp.sh 0.2` |
| T1 | `SX_VERBOSE=1 python3 experiment.py 4 2>&1 \| tee /tmp/exp4.log` |
| T4 | stack sampler + ktrace |

### 4.2 Exact commands

```sh
# T4 — run the sampler as soon as the PID appears; it spans the whole experiment
PID=$(pgrep -n exchange_server)
while :; do date '+%H:%M:%S'; procstat -k $PID; sleep 0.5; done | tee /tmp/exp4.kstack

# in a fifth shell, or after the fact:
kdump -f /tmp/exp4.kt -T | egrep 'kevent|recvfrom|accept|sendto' | tail -40
# (start it with: ktrace -i -f /tmp/exp4.kt -p $PID -t c , stop with ktrace -C)

# fd ↔ client mapping while both are connected
~/tools/fdmap.sh
netstat -an -p tcp | egrep 'Proto|\.5000'
```

### 4.3 Observations to capture

**Latency (T1)** — the headline number:

```
Client 2 response: 'OK'
Elapsed time: 0.000xxx seconds
```

**Kernel stack (T4)** — every sample, before and after Client 2 arrives, ends in the kqueue path,
never in the socket-receive path:

```
  PID    TID COMM             TDNAME           KSTACK
 <N> 100xxx exchange_server   -                mi_switch sleepq_switch sleepq_catch_signals
                                               sleepq_wait_sig _cv_wait_sig kqueue_scan
                                               kern_kevent_fp kern_kevent sys_kevent
                                               amd64_syscall fast_syscall_common
```

The diagnostic contrast to state explicitly: a thread-blocked-in-`recv` server would instead show
`sbwait`/`soreceive_generic`/`kern_recvit`/`sys_recvfrom` in that stack. Point at the absence of
`sbwait` — that is the evidence.

**Server log (T1)** — one `[recv] fd=5 n=20 data=LOGIN blocked_client` for Client 1 and nothing
further from it; then `accepted fd=6` and `[recv] fd=6 n=19` followed by the `OK` for Client 2.

**Kernel queues (T2)** — Client 1's connection shows `Recv-Q 0`, `Send-Q 0`, `ESTABLISHED`, exactly
like Client 2's. The idle connection is indistinguishable from any other at the TCP layer; its
incompleteness exists only in the server's `LineBuffer`.

**Wire (T3)** — after Client 1's 20-byte segment and its ACK, there is no further traffic on that
4-tuple; Client 2's SYN, data and the `OK` reply flow undisturbed within the same millisecond.

### 4.4 Answer

Yes — the server accepts and services Client 2 with sub-millisecond latency while Client 1 sits
connected and silent. The operation that decides this is the **readiness-notification call**:
`kevent()` with a NULL timeout, which blocks on the *set* of registered descriptors and returns
only those that are actually ready. `procstat -k` shows the single server thread sleeping in
`kqueue_scan`/`kern_kevent` for the entire run, never in `soreceive`/`sbwait`. Client 1's socket
simply never becomes readable, so it is never in the returned event list and costs nothing; its
20 partial bytes were already copied into userspace and wait there as an incomplete line. Had the
server instead called a blocking `recv()` on Client 1's descriptor — or accepted and served
connections sequentially — the thread would have slept in `sbwait` inside that one socket's
receive path, and Client 2's `LOGIN` would have gone unanswered until Client 1 sent its newline.
Non-blocking descriptors plus readiness notification decouple the server's progress from any
individual client's behaviour.

*(Optional, strengthens the answer materially: add a `--blocking` mode to the server that accepts
one connection and loops on a blocking `recv()`, run the same experiment against it, and show
`procstat -k` parked in `sbwait` with Client 2's elapsed time growing without bound. A
side-by-side of the two stacks is the most convincing possible deliverable for this experiment.)*

---

# Experiment 5 — Multiple Clients and I/O Multiplexing (optional)

Harness behaviour: five connections; clients 1, 3 and 5 each send `LOGIN client_N\n` (16 bytes)
with 0.5 s spacing; clients 2 and 4 stay silent; 15 s observation window.

### 5.1 Terminal setup & strategy

Readiness here is *transient*: the server drains each socket within microseconds of it becoming
readable, so polling `Recv-Q` will almost always show 0 and cannot by itself identify the ready
sockets. The rigorous approach is therefore threefold: (a) a syscall trace that records which
descriptors `kevent()` actually caused the server to read; (b) an fd↔ephemeral-port map from
`procstat -f`/`sockstat` cross-referenced against the harness's printed source ports; (c) a
high-frequency `Recv-Q` poller to try to catch the readiness window, presented as corroboration
rather than as the primary evidence. Explaining *why* (a) is stronger than (c) is itself worth
marks.

| Terminal | Command |
|---|---|
| T2 | `~/tools/poll-tcpx.sh 0.1 /tmp/exp5.tcpx` **and** `~/tools/poll-tcp.sh 0.05 /tmp/exp5.states` |
| T3 | `tcpdump -i lo0 -n -S -tttt 'tcp port 5000' \| tee /tmp/exp5.txt` |
| T1 | `SX_VERBOSE=1 python3 experiment.py 5 2>&1 \| tee /tmp/exp5.log` |
| T4 | ktrace + fd map |

### 5.2 Exact commands

```sh
# T4 — attach the trace immediately after the five connects, before the LOGINs
PID=$(pgrep -n exchange_server)
ktrace -i -f /tmp/exp5.kt -p $PID -t c

~/tools/fdmap.sh                       # fd -> 127.0.0.1:5000 127.0.0.1:<ephemeral>
procstat -f $PID | grep TCP | tee /tmp/exp5.fdmap
sockstat -4 -c -p 5000

# after the three LOGINs
ktrace -C
kdump -f /tmp/exp5.kt -T | egrep 'kevent|recvfrom' | tee /tmp/exp5.syscalls
kdump -f /tmp/exp5.kt -T | grep recvfrom | awk '{print $NF, $0}' | grep -o 'recvfrom(0x[0-9a-f]*' | sort | uniq -c
```

Correlate the three tables by hand into a single figure for the report:

| Harness line | Ephemeral port | Server fd (`procstat -f`) | Appears in `recvfrom` trace? |
|---|---|---|---|
| Client 1 | `<E1>` | 5 | yes, `RET recvfrom 16` |
| Client 2 | `<E2>` | 6 | no |
| Client 3 | `<E3>` | 7 | yes |
| Client 4 | `<E4>` | 8 | no |
| Client 5 | `<E5>` | 9 | yes |

### 5.3 Observations to capture

- `procstat -f $PID` showing five connected TCP descriptors plus fd 3 (listener) and fd 4 (kqueue)
  — one screenshot proving the server holds all five simultaneously with a single thread.
- `kdump` output showing `kevent` returning, then `recvfrom` on **only** fds 5, 7 and 9, each
  returning 16, each followed by a `RET recvfrom -1 errno 35` (EAGAIN) that terminates the drain
  loop. Descriptors 6 and 8 never appear in a `recvfrom` line at all.
- Server log lines `[recv] fd=5 n=16 data=LOGIN client_1` etc., three of them, 0.5 s apart.
- `netstat -an -p tcp -x` showing all five connections with identical `R-HIWA`/`S-HIWA` socket
  buffer high-water marks and `rcvtime` differing between the active and idle ones — the idle
  connections' `rcvtime` (time since last receive) keeps growing while the active ones reset.
- If your 0.05 s poller happened to catch it, one snapshot with `Recv-Q 16` on one connection. Say
  plainly in the report if it did not, and explain that the server drains the buffer within
  microseconds of the wakeup.

### 5.4 Answer

At the instant each `LOGIN` arrives, exactly one connection is ready: the sockets of clients 1, 3
and 5 in turn, identified by ephemeral ports `<E1>`, `<E3>`, `<E5>` and server descriptors 5, 7 and
9. The determining evidence is the syscall trace: `kevent()` returns and the server issues
`recvfrom()` only on those descriptors, each returning 16 bytes and then `EAGAIN`; descriptors 6
and 8 (clients 2 and 4) are registered with `EVFILT_READ` but never reported ready, so the server
never syscalls on them at all. `netstat -x`'s per-connection `rcvtime` corroborates this, and
`procstat -f` proves a single thread is holding all five descriptors concurrently. This is the
central property of readiness-based multiplexing: the cost of an idle connection is a `knote` and
a socket buffer in the kernel, not a thread, a stack, or a wasted syscall — the kernel evaluates
readiness and returns only the subset that can make progress.

---

# Experiment 6 — FIN vs. RST: Orderly and Abrupt Termination

Harness behaviour: **Part A** — connect, `shutdown(SHUT_WR)` (half-close, FIN), wait 10 s, then
`close()`. **Part B** — connect, set `SO_LINGER{onoff=1, linger=0}`, `close()` → RST. Then 15 s.

### 6.1 Terminal setup & strategy

Both parts must be captured in a single continuous trace so the two teardowns can be compared side
by side on one timeline. Remember the harness's startup probe already produced one RST before Part
A — annotate it and exclude it. The server-side evidence differs by design in your implementation:
Part A hits the `n == 0` branch, Part B hits the `ECONNRESET` branch, and each prints a distinct
log line.

| Terminal | Command |
|---|---|
| T3a | `tcpdump -i lo0 -n -S -vv -tttt 'tcp port 5000' \| tee /tmp/exp6.txt` |
| T3b | `tcpdump -i lo0 -n -s0 -w /tmp/exp6.pcap 'tcp port 5000'` |
| T2 | `~/tools/poll-tcp.sh 0.1 /tmp/exp6.states` |
| T1 | `SX_VERBOSE=1 python3 experiment.py 6 2>&1 \| tee /tmp/exp6.log` |
| T4 | per-phase snapshots + ktrace |

### 6.2 Exact commands

```sh
PID=$(pgrep -n exchange_server)
ktrace -i -f /tmp/exp6.kt -p $PID -t c

# during Part A's 10 s window
netstat -an -p tcp | egrep 'Proto|\.5000'
procstat -f $PID | grep TCP

# immediately after the harness prints "The client will now close abortively."
netstat -an -p tcp | egrep 'Proto|\.5000'
procstat -f $PID | grep TCP

ktrace -C
kdump -f /tmp/exp6.kt -T | egrep 'kevent|recvfrom|shutdown|close' | tail -40

# isolate each teardown from the capture for the report
tcpdump -r /tmp/exp6.pcap -n -S -tttt 'tcp[tcpflags] & (tcp-fin|tcp-rst) != 0'
grep -E 'peer closed|reset by peer' /tmp/exp6.log
```

### 6.3 Observations to capture

**Part A (orderly).** Packets:

```
IP 127.0.0.1.<E1> > 127.0.0.1.5000: Flags [F.], seq 1, ack 1        <- shutdown(SHUT_WR)
IP 127.0.0.1.5000 > 127.0.0.1.<E1>: Flags [F.], seq 1, ack 2        <- server ACK+FIN
IP 127.0.0.1.<E1> > 127.0.0.1.5000: Flags [.],  ack 2
```
Server log: `[server] fd=5: peer closed (FIN, recv==0)` then `[server] closing fd=5 owner=N`.
`kdump`: `RET recvfrom 0` → `CALL shutdown(0x5,1)` → `CALL close(0x5)`.
States: the client endpoint sits in **TIME_WAIT** for the rest of the run (it was in `FIN_WAIT_2`
for the microseconds between its FIN and the server's); the server endpoint vanishes. Screenshot
`netstat` here — this TIME_WAIT is easy to capture and is the same state Experiment 2 asks about.

**Part B (abortive).** A single packet, no handshake of any kind:

```
IP 127.0.0.1.<E2> > 127.0.0.1.5000: Flags [R.], seq 1, ack 1, win 0, length 0
```
Server log: `[server] fd=6: connection reset by peer (RST, ECONNRESET)`.
`kdump`: `RET recvfrom -1 errno 54 Connection reset by peer` (FreeBSD `ECONNRESET` = 54).
States: **both** endpoints disappear from `netstat` immediately. There is no `FIN_WAIT`, no
`LAST_ACK`, and critically **no TIME_WAIT** — screenshot the absence and contrast it with Part A's
surviving TIME_WAIT entry in the same terminal.

A clean one-frame comparison for the report:

```sh
tcpdump -r /tmp/exp6.pcap -n -S -tttt | egrep 'Flags \[F|Flags \[R'
```

### 6.4 Answer

Orderly termination is a *negotiated, in-band* end of stream: `shutdown(SHUT_WR)` sends FIN, which
occupies one sequence number, is acknowledged, and is delivered to the peer application as
`recv() == 0`; any data already queued is still delivered first, the connection is closed in each
direction independently, and the initiator holds `TIME_WAIT` for 2×MSL so that a delayed duplicate
segment cannot be misapplied to a later incarnation of the same 4-tuple. Abortive termination via
`SO_LINGER{1,0}` sends a single RST: it is *out of band*, carries no sequence semantics, is never
acknowledged, discards any queued data in both socket buffers, tears the PCB down at both ends
immediately, and leaves no TIME_WAIT. From the server's socket, the two look entirely different:
the FIN surfaces as a readable event whose `recv()` returns 0, while the RST surfaces as
`EVFILT_READ` with `EV_EOF` set and a `recv()` failing with `ECONNRESET` (errno 54) — which is why
the server distinguishes and logs them separately, and why writing to a reset connection would
raise `EPIPE` (harmless here because `SIGPIPE` is ignored).

---

# Experiment 7 — Backpressure and the Slow Receiver

Harness behaviour: two market-data subscribers to JNST — one drained continuously, one **never
read** — plus two traders submitting up to 5000 matching pairs at ~1 ms intervals. Each trade emits
`TRADE JNST 1 238\n` = 17 bytes to each subscriber, so the slow socket must absorb up to ~85 KB.

### 7.1 Pre-flight: make the phenomenon visible

With FreeBSD's default auto-tuned buffers (`recvspace=65536` growing toward `recvbuf_max`, often
2 MB), 85 KB may be swallowed entirely and you will never see a zero window. Shrink the buffers
first, and **record both the before and after values** — the comparison is itself an excellent
result:

```sh
sysctl net.inet.tcp.sendspace net.inet.tcp.recvspace \
       net.inet.tcp.sendbuf_auto net.inet.tcp.recvbuf_auto     # BEFORE, screenshot this

sysctl net.inet.tcp.sendbuf_auto=0
sysctl net.inet.tcp.recvbuf_auto=0
sysctl net.inet.tcp.sendspace=4096
sysctl net.inet.tcp.recvspace=4096
```

Restore afterwards. Run the experiment once with defaults and once with the small buffers if you
have time; the default run demonstrates auto-tuning absorbing the burst, the tuned run
demonstrates the full flow-control mechanism.

### 7.2 Terminal setup & strategy

Four simultaneous evidence streams: the wire (zero-window advertisements and persist probes), the
kernel queues (`Send-Q` on the server side, `Recv-Q` on the slow client side), the application
(the `[flush] ... EAGAIN` line at the exact moment the write refuses), and the liveness of the
normal client (harness trade counter still advancing, server CPU low).

| Terminal | Command |
|---|---|
| T2 | `~/tools/poll-tcpx.sh 0.5 /tmp/exp7.tcpx` and `~/tools/poll-tcp.sh 0.5 /tmp/exp7.states` |
| T3 | `tcpdump -i lo0 -n -S -vv -tttt -B 4096 'tcp port 5000' \| tee /tmp/exp7.txt` |
| T1 | `SX_VERBOSE=1 python3 experiment.py 7 2>&1 \| tee /tmp/exp7.log` |
| T4 | `top -b -d 60 -s 1 -p $(pgrep -n exchange_server)` plus snapshots |

### 7.3 Exact commands

```sh
PID=$(pgrep -n exchange_server)

# identify which ephemeral port is which client from the harness output, then:
netstat -an -p tcp    | egrep 'Proto|\.5000'
netstat -an -p tcp -x | egrep 'Proto|\.5000'        # R-HIWA/S-HIWA, R-BCNT/S-BCNT, persist timer
procstat -f $PID | grep TCP

# the moment backpressure begins, in the application:
grep '\[flush\]' /tmp/exp7.log | head

# zero windows and persist probes, isolated:
grep -n 'win 0' /tmp/exp7.txt | head
tcpdump -r /tmp/exp7.pcap -n -S -tttt 'tcp port 5000 and tcp[tcpflags] & tcp-push != 0 and less 60'

# CPU / liveness
top -b -s 1 -n 3 | head -20
```

### 7.4 Observations to capture

**Kernel queues** — the defining screenshot, one `netstat -an -p tcp` frame showing the two
subscribers side by side:

```
tcp4  0      <big>  127.0.0.1.5000   127.0.0.1.<SLOW>    ESTABLISHED   <- server Send-Q non-zero
tcp4  <big>  0      127.0.0.1.<SLOW> 127.0.0.1.5000      ESTABLISHED   <- slow client Recv-Q full
tcp4  0      0      127.0.0.1.5000   127.0.0.1.<NORM>    ESTABLISHED   <- normal client, both zero
```

**Extended view** — `netstat -an -p tcp -x` for the slow connection shows `R-BCNT` approaching
`R-HIWA`, and a non-zero **`persist`** timer column once the window closes. That persist counter is
the single best piece of evidence in this experiment; most submissions never find it.

**Wire** — three things in sequence, each worth its own screenshot or a grep:
1. the receiver's window shrinking on successive ACKs (`win 1024`, `win 512`, …);
2. a pure ACK with `win 0` from the slow client — the **zero-window advertisement**;
3. periodic 1-byte **window probes** from the server (persist timer, exponentially backing off),
   each answered with another `win 0`.

**Application** — the `[flush]` line, timestamped against the pcap:

```
[flush] fd=6 send() EAGAIN, 17 bytes still queued, enabling EVFILT_WRITE
[flush] fd=6 send() EAGAIN, 12nnn bytes still queued, enabling EVFILT_WRITE
```

**Isolation of the failure** — in the same frames, the normal client's connection shows
`Send-Q 0` throughout and continuous data segments in `tcpdump`; T1 reaches
`Generated 5000 matching trades`; `top` shows `exchange_server` at low CPU and no growth in RES
beyond the queued bytes. If you kept the 8 MB cap and pushed the trade count high enough to reach
it, you would additionally see `output backlog ... exceeds cap, dropping client` — state the policy
and whether it triggered (with 5000 trades ≈ 85 KB, it does not).

### 7.5 Answer

As the slow client stops reading, its socket receive buffer fills (`Recv-Q` rises to the
high-water mark) and its ACKs advertise a shrinking window until it advertises **zero**. TCP flow
control then forbids the server's stack from sending further data, so the server's socket send
buffer fills (`Send-Q` rises) and the server enters the persist state, emitting periodic window
probes. At that point `send()` on the non-blocking descriptor returns `EAGAIN`; the server keeps
the undelivered bytes in that connection's own `OutBuffer`, enables `EVFILT_WRITE` for that fd
only, and returns to the event loop. The backpressure is therefore confined to the offending
connection: throughout, the normal subscriber's `Send-Q` stays at 0, it continues receiving every
`TRADE`, and the traders keep getting responses. Had the socket been blocking, that same `send()`
would have slept inside the kernel and the single-threaded server would have stalled completely
for every client until the slow reader drained — which is precisely the failure mode this design
avoids. The residual risk is memory, not stalling, which is why the per-connection output buffer is
capped at 8 MB and a client exceeding it is disconnected.

---

# Experiment 8 — Unexpected Client Disconnection

Harness behaviour: a **separate helper process** subscribes to JNST and then sleeps forever without
reading; a second subscriber in the harness keeps draining; 20 trades are generated; the helper is
killed with **`SIGKILL`** (`killpg`); 50 more trades follow at 50 ms intervals; then a 15 s
observation window.

### 8.1 Terminal setup & strategy

The critical subtlety: at the moment of death the helper's receive buffer contains **unread data**
(≈20 × 17 bytes of `TRADE` messages it never read). BSD semantics say that closing a socket with
unread data in its receive buffer produces an **RST**, not a FIN. So predict an RST, then prove it
— and explain what would have happened had the buffer been empty. Capture the exact moment by
timestamping the kill against the pcap.

| Terminal | Command |
|---|---|
| T3a | `tcpdump -i lo0 -n -S -vv -tttt 'tcp port 5000' \| tee /tmp/exp8.txt` |
| T3b | `tcpdump -i lo0 -n -s0 -w /tmp/exp8.pcap 'tcp port 5000'` |
| T2 | `~/tools/poll-tcp.sh 0.2 /tmp/exp8.states` and `~/tools/poll-tcpx.sh 0.5` |
| T1 | `SX_VERBOSE=1 python3 experiment.py 8 2>&1 \| tee /tmp/exp8.log` |
| T4 | process + fd inspection, ktrace |

### 8.2 Exact commands

```sh
PID=$(pgrep -n exchange_server)
ktrace -i -f /tmp/exp8.kt -p $PID -t c

# BEFORE the kill: identify the helper and its socket
pgrep -lf 'python3 -u -c'                 # the disappearing Market-Data client
HELPER=$(pgrep -n -f 'python3 -u -c'); echo $HELPER
sockstat -4 -p 5000                       # shows BOTH python processes and the server
procstat -f $PID | grep TCP | tee /tmp/exp8.fdmap.before
netstat -an -p tcp | egrep 'Proto|\.5000'

# AFTER the harness prints "The client process has disappeared."
ps -p $HELPER                             # gone
netstat -an -p tcp | egrep 'Proto|\.5000' # the dead 4-tuple is gone; the survivor remains
procstat -f $PID | grep TCP | tee /tmp/exp8.fdmap.after
diff /tmp/exp8.fdmap.before /tmp/exp8.fdmap.after

ktrace -C
kdump -f /tmp/exp8.kt -T | egrep 'kevent|recvfrom|sendto|close' | tail -40
grep -E 'reset by peer|peer closed|closing fd' /tmp/exp8.log

# the termination packet, isolated
tcpdump -r /tmp/exp8.pcap -n -S -tttt 'tcp[tcpflags] & (tcp-rst|tcp-fin) != 0'
```

Also record the keepalive baseline, because it explains the counterfactual:

```sh
sysctl net.inet.tcp.always_keepalive net.inet.tcp.keepidle net.inet.tcp.keepintvl
```

### 8.3 Observations to capture

- **Before**: `sockstat -4 -p 5000` listing the server plus *both* python clients, and
  `procstat -f $PID` showing both market-data descriptors — establish the baseline.
- **The termination packet**: an `[R]`/`[R.]` segment from the helper's ephemeral port immediately
  after the kill, with no preceding FIN. Timestamp it against T1's "has disappeared" line.
- **Server detection**: `[server] fd=N: connection reset by peer (RST, ECONNRESET)` followed by
  `[server] closing fd=N`, and in `kdump` either `RET recvfrom -1 errno 54` or, if the server was
  mid-publish, `RET sendto -1 errno 32` (`EPIPE`) — note which one your run produced and why the
  process did not die (`SIGPIPE` ignored).
- **State tables**: the dead connection absent from `netstat` on both sides with no TIME_WAIT
  (contrast with Experiment 2), and the `diff` of the before/after `procstat -f` output showing
  exactly one descriptor removed.
- **Survivor unaffected**: the surviving subscriber's connection still `ESTABLISHED`, `Send-Q 0`,
  and `tcpdump` showing it continuing to receive `TRADE` segments through the 50 post-kill trades;
  T1 completes without error. This is the "one client's failure must not affect others" requirement
  discharged.

### 8.4 Answer

`SIGKILL` gives the application no chance to run shutdown code, but the kernel still reclaims the
process's descriptors, so the socket is closed by the OS on the dying process's behalf. Because the
helper had never read the market-data updates the server sent it, its receive buffer held unread
data at close time, and BSD-derived stacks answer that case with an **RST** rather than a FIN — a
single unacknowledged segment that immediately destroys the PCB at both ends, which is why the
connection vanishes from `netstat` with no FIN exchange and no TIME_WAIT. The server learns of the
failure at its next interaction with that socket: `kqueue` reports `EVFILT_READ` with `EV_EOF` and
`recv()` fails with `ECONNRESET` (errno 54), or a concurrent publish fails with `EPIPE` (errno 32);
either path routes into `close_conn()`, which releases the descriptor, the username and the
`owner_id → fd` mapping while deliberately leaving any resting orders in the book. Had the buffer
been empty, the same kernel-initiated close would have produced an ordinary FIN and the server
would have seen `recv() == 0` instead — the distinction is the state of the receive buffer, not the
signal. Note also that detection here is immediate only because the server is actively writing to
that socket; a *silently* vanished peer on an idle connection would not be detected until TCP
keepalives fired, `net.inet.tcp.keepidle` being 7200000 ms (2 hours) by default.

---

## 9. Report assembly checklist

For each experiment, the report section should contain, in this order:

1. **Question** (quoted from the handout) and your one-paragraph **answer**.
2. **Approach** — 3–4 sentences on which layer each tool observes: `tcpdump` = wire, `netstat`
   = PCB/socket-buffer state, `sockstat`/`procstat -f` = process↔socket binding, `procstat -k`
   = where the thread is sleeping, `ktrace`/`kdump` = the syscall sequence.
3. **Commands** — verbatim, including the polling loops.
4. **Screenshots**, each with a one-line caption naming what to look at (`"Send-Q = 41216 on the
   slow subscriber while the normal subscriber holds 0"`), not bare images.
5. **Anomalies and honesty notes** — the harness's startup RST; states too transient to capture and
   how you proved them anyway; sysctls you changed and restored.

Cross-cutting points to make somewhere in the report, since they are what the 40% investigation
weight is really testing:

- A socket is identified by a 4-tuple; a listener is a degenerate case with a wildcard peer (Exp 1).
- TCP has no message boundaries; framing is an application responsibility (Exp 3).
- Readiness notification decouples server progress from individual client behaviour (Exp 4, 5).
- Termination has two distinct forms with different state-machine and data-delivery semantics
  (Exp 2, 6, 8).
- Flow control is a per-connection kernel mechanism that surfaces to the application as `EAGAIN`,
  and the application's job is to hold the surplus and wait for writability (Exp 7).
