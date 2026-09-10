# Bonus §6.9 — Connection Scalability and I/O Design
### 70,000 simultaneous idle TCP connections on FreeBSD 14.4 · kqueue vs thread-per-connection

This follows the same four-section structure as the Experiments 1–8 guide: terminal setup and
strategy, exact commands, observations to capture, and the written answer. The difference is that
the Bonus is a *measurement ladder* — the same procedure repeated at seven load points — so §B.4
is a single procedure you execute seven times, and §B.7 answers the five questions the handout
asks.

---

## B.0 The three facts that decide whether this works at all

Read these before touching the VM. Most attempts at this bonus fail on the second one and
misdiagnose it as a memory problem.

**1. Every connection costs this machine two sockets, not one.** Server and generator both run
inside the VM, so 70,000 connections means 140,000 TCP PCBs, 140,000 `struct socket`s and ~140,000
open file descriptors *system-wide*. Every limit you tune must be sized against 140k, not 70k. When
you count with `netstat`, each connection also appears **twice** — filter on the server's local port
or you will report double.

**2. You cannot reach 70,000 connections to one `(dst_ip, dst_port)` from one source address.** A
TCP connection is identified by a 4-tuple. With `127.0.0.1:5000` fixed as the destination and
`127.0.0.1` as the source, the only free field is the client's ephemeral port, and FreeBSD's default
range `net.inet.ip.portrange.first=10000 … last=65535` gives ~55,535 — and even the full
1024–65535 range gives only ~64,500. **The ceiling is architectural, not a resource limit.** The fix
is to widen the tuple space, which is why `connflood` takes `--src`: add loopback aliases and
round-robin the source address so each alias contributes its own port range.

```sh
ifconfig lo0 alias 127.0.0.2/32
ifconfig lo0 alias 127.0.0.3/32
ifconfig lo0                       # screenshot this: three addresses on lo0
```

Two aliases plus `127.0.0.1` gives ~193,000 possible tuples against a single server port. Discovering
this limit deliberately (run once *without* `--src`, hit `EADDRNOTAVAIL`, then fix it) is worth
more marks than avoiding it, so §B.5 has you do exactly that.

**3. Give the VM 4 GB RAM and 2 cores.** The handout's 2 GB minimum is sized for the main
assignment. 140k sockets at roughly 2 KB of kernel memory each is ~300 MB before userspace, and you
want headroom to distinguish "the design is expensive" from "the VM was starved".

---

## B.1 Kernel tuning

### B.1.1 Boot-time tunables — `/boot/loader.conf`, then reboot

`kern.ipc.maxsockets` is read-only at runtime (`RDTUN`); trying to `sysctl` it will fail, which is a
common source of confusion.

```
# /boot/loader.conf
kern.maxfiles="400000"
kern.ipc.maxsockets="300000"
kern.ipc.nmbclusters="262144"
```

```sh
shutdown -r now
# after reboot, verify and screenshot:
sysctl kern.maxfiles kern.ipc.maxsockets kern.ipc.nmbclusters kern.maxfilesperproc
```

### B.1.2 Runtime sysctls — set before every run

```sh
sysctl kern.maxfiles=400000
sysctl kern.maxfilesperproc=200000
sysctl kern.ipc.somaxconn=4096          # alias of kern.ipc.soacceptqueue on 14.x
sysctl net.inet.ip.portrange.first=1024
sysctl net.inet.ip.portrange.last=65535
sysctl net.inet.ip.portrange.randomized=0   # sequential allocation; randomized search
                                            # degrades badly once the range is nearly full
sysctl net.inet.tcp.syncache.hashsize       # record it; SYN bursts land here first
```

Record every before/after value — the report needs to show the environment was tuned deliberately,
not accidentally.

### B.1.3 Per-process descriptor limits

Both the server shell and the generator shell need this, in *each* terminal before launching:

```sh
ulimit -n 200000        # FreeBSD sh; fails silently if kern.maxfilesperproc is lower
ulimit -n               # confirm
```

`connflood` additionally calls `setrlimit(RLIMIT_NOFILE, rlim_max)` itself and prints the result, so
its first log line documents the limit it actually got.

### B.1.4 One server-side patch

`src/netutil.hpp` currently passes `SOMAXCONN` (the compile-time constant 128) to `listen()`. Under
a 16,000 conn/s burst that backlog overflows and the generator sees `ECONNREFUSED`. Change it:

```cpp
    if (listen(fd, 4096) < 0) {          // was SOMAXCONN; kernel clamps to kern.ipc.soacceptqueue
```

Nothing else in the server changes — that is the point of the exercise.

---

## B.2 Build the bonus tools

```sh
c++ -std=c++17 -O2 -Wall -o connflood       tools/connflood.cpp
c++ -std=c++17 -O2 -Wall -o threaded_server tools/threaded_server.cpp -lpthread
chmod +x tools/measure.sh
make clean && make                      # rebuild the server with the listen() change
```

Or add to the `Makefile`:

```make
bonus: connflood threaded_server

connflood: tools/connflood.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

threaded_server: tools/threaded_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

.PHONY: bonus
```

`connflood` is the deliverable the handout asks for (item 1 of §6.9.1). `threaded_server` is a
comparison artifact for questions 3–5 and is **not** part of the graded Exchange Server — say so in
the README so it is not mistaken for your submission using threads.

---

## B.3 Terminal setup & strategy

The strategy is a controlled ladder: hold the server constant, raise the connection count in seven
steps, and at each step take one atomic snapshot across four independent accounting layers —
userspace RSS (`ps`), kernel slab (`vmstat -z`), descriptor tables (`procstat -f`, `kern.openfiles`)
and network buffers (`netstat -m`). Every measurement must be taken **while the connections are
established and idle**, which is why `connflood` parks and holds rather than exiting.

| Terminal | Role | Command |
|---|---|---|
| **T1** | Exchange Server | `ulimit -n 200000; ./server/run-server 127.0.0.1 5000` |
| **T2** | Generator | `ulimit -n 200000; ./connflood 127.0.0.1 5000 <N> --src 127.0.0.1,127.0.0.2,127.0.0.3` |
| **T3** | Measurement | `tools/measure.sh <N>` — run once the generator prints "holding connections open" |
| **T4** | Live watch | `top -o res` and `~/tools/poll-tcp.sh 2` (optional; adds context) |

Take a **baseline with zero connections first** — every kernel-memory number in the table is only
meaningful as a delta from it:

```sh
tools/measure.sh baseline_0conn | tee /tmp/bonus_baseline.txt
vmstat -z > /tmp/zones_baseline.txt
```

---

## B.4 The measurement procedure (repeat for N = 10k … 70k)

```sh
# T1 — restart the server clean for each rung so RSS is not cumulative
pkill exchange_server; ulimit -n 200000; ./server/run-server 127.0.0.1 5000

# T2
ulimit -n 200000
./connflood 127.0.0.1 5000 10000 --src 127.0.0.1,127.0.0.2,127.0.0.3 --report 2500 \
  2>&1 | tee /tmp/gen_10000.log
#   wait for:  [gen] target reached
#              [gen] holding connections open and idle. Take measurements now.

# T3 — while it holds
tools/measure.sh 10000 | tee /tmp/measure_10000.txt

# then Ctrl-C the generator, confirm teardown, and move to the next rung
netstat -an -p tcp | awk '$4 ~ /\.5000$/ && $6=="ESTABLISHED"' | wc -l
```

Repeat with `20000`, `30000`, `40000`, `50000`, `60000`, `70000`. `tools/measure.sh` appends a CSV
row to `/tmp/bonus.csv` each time, which becomes your table directly.

**Sanity expectation.** On a reference run at 3,000 connections, the server held 3,005 descriptors
(3,000 conns + listener + kqueue + stdio), RSS was ~4.4 MB and CPU 0.7% during setup and ~0% once
idle, with the generator establishing at ~16,000 conn/s. Scale linearly from there for your
predictions; if your 10k numbers are wildly off that trajectory, something is wrong before you climb
higher.

**Deep measurement of kernel cost per connection** — this is what separates a strong bonus from an
average one. `ps` RSS only shows *userspace*; the majority of a connection's cost is kernel slab:

```sh
vmstat -z > /tmp/zones_70000.txt
# per-zone delta, in bytes, between baseline and loaded:
awk -F'[,:]' 'NR>2 && NF>3 {gsub(/ /,"",$1); print $1, $2, $4}' /tmp/zones_baseline.txt > /tmp/zb
awk -F'[,:]' 'NR>2 && NF>3 {gsub(/ /,"",$1); print $1, $2, $4}' /tmp/zones_70000.txt   > /tmp/zl
join /tmp/zb /tmp/zl | awk '{d=$5-$3; if (d>0) printf "%-22s size=%-6s used_delta=%-8d bytes=%d\n",$1,$2,d,d*$2}' | sort -k4 -t= -n
```

Zones that will move: `socket`, `tcpcb`, `tcp_inpcb`, `knote`, plus the file-descriptor zone. Sum
their byte deltas, divide by the connection count, and you have **kernel bytes per idle
connection** — a number you can put in the report and defend in the viva.

---

## B.5 The deliberate failure run (do this, don't skip it)

Run once **without** `--src` to expose the architectural ceiling:

```sh
./connflood 127.0.0.1 5000 70000 --report 5000 2>&1 | tee /tmp/gen_noalias.log
```

Expected: the generator climbs smoothly, then stops with

```
[gen] STOPPED: connect(): EADDRNOTAVAIL (ephemeral port range exhausted) (errno 49: Can't assign requested address)
[gen] established          : 6xxxx
```

Screenshot it alongside `sysctl net.inet.ip.portrange.first net.inet.ip.portrange.last`. Then re-run
*with* the aliases and show it passing straight through that number. This one comparison
demonstrates you understand that TCP connection identity — not memory — sets the first ceiling.

---

## B.6 The concurrency-design comparison (questions 3–5)

Run the *identical* generator against the thread-per-connection server to quantify the trade-off.
First record the ceiling the OS imposes on threads:

```sh
sysctl kern.threads.max_threads_per_proc kern.threads.max_threads_hits
sysctl kern.maxproc
```

Then:

```sh
# T1
ulimit -n 200000; ./threaded_server 127.0.0.1 5001
# T2
ulimit -n 200000; ./connflood 127.0.0.1 5001 70000 --src 127.0.0.1,127.0.0.2,127.0.0.3 \
  --report 250 2>&1 | tee /tmp/gen_threaded.log
# T3 — measure at whatever count it reaches, and again at 1000 for a like-for-like comparison
ps -o pid=,rss=,vsz=,nlwp=,%cpu= -p $(pgrep -n threaded_server)
```

The threaded server prints the exact failure point:

```
[threaded] pthread_create FAILED at 1500 live threads: Resource temporarily unavailable (rc=35)
```

Then quantify the mitigation by shrinking the stack, which is the "if you change your
implementation, measure again" part of question 5:

```sh
./threaded_server 127.0.0.1 5001 64        # 64 KB stacks instead of the default
```

Build a like-for-like table at a count both designs can reach (1,000 is safe):

| Design | Conns | RSS | VSZ | Threads (`nlwp`) | Idle CPU | Fails at |
|---|---|---|---|---|---|---|
| kqueue, single thread | 1,000 | | | 1 | | — |
| thread-per-conn, default stack | 1,000 | | | 1,001 | | |
| thread-per-conn, 64 KB stack | 1,000 | | | 1,001 | | |

`VSZ` is the striking column: virtual address space grows by the stack size per connection even
though RSS does not, and `kern.threads.max_threads_per_proc` caps the design long before memory
does.

---

## B.7 Deliverables and the written answers

### B.7.1 The table (handout deliverable 2)

Fill from `/tmp/bonus.csv`. Add the kernel-memory column from §B.4 — the handout does not ask for it
explicitly, and including it is exactly the kind of thing the 40% investigation weight rewards.

| Idle conns | Server RSS | Server kernel mem (Δ UMA) | Server CPU | Server FDs | System-wide FDs (`kern.openfiles`/`maxfiles`) | Socket-buffer usage (`netstat -m` clusters / limit) | Max established |
|---|---|---|---|---|---|---|---|
| 10,000 | | | | | | | |
| 20,000 | | | | | | | |
| 30,000 | | | | | | | |
| 40,000 | | | | | | | |
| 50,000 | | | | | | | |
| 60,000 | | | | | | | |
| 70,000 | | | | | | | |

### B.7.2 Screenshots (handout deliverable 3 — required at 10k, 40k, 70k)

Each screenshot must show the connection count **and** the measurements in the same frame, which is
what `tools/measure.sh` is built to produce. Capture per rung:

1. the `measure.sh` block, with the `established to server port 5000` line and the resource lines visible;
2. the generator's summary block (`established: N`, `target reached`);
3. `top -o res` showing `exchange_server` RES and CPU with N connections live.

Plus, once each: `ifconfig lo0` with the aliases, the `EADDRNOTAVAIL` failure from §B.5, and the
`pthread_create FAILED` line from §B.6.

### B.7.3 The five answers

**Q1 — Can the server maintain all requested connections at each count, and where does it first
fail?** Report your actual ladder. The expected shape: with default tunables it fails at
~55,000 (`EADDRNOTAVAIL`, ephemeral ports) or earlier at `EMFILE` if `ulimit -n` was left at its
default; after widening `portrange`, adding two `lo0` aliases and raising `kern.maxfiles`,
`kern.maxfilesperproc` and `kern.ipc.maxsockets`, all seven rungs including 70,000 hold. Give the
first failure as an errno and a count, not as "it got slow".

**Q2 — First significant bottleneck, supported by the table.** Argue from the columns. Server CPU
stays at ~0% while idle across every rung, which eliminates CPU. `netstat -m` cluster usage stays
near baseline because an idle connection allocates no mbufs — socket buffer *limits* (`sb_hiwat`) are
accounting numbers, not allocations — which eliminates socket-buffer memory. RSS grows linearly at
roughly 1.5 KB per connection and remains a small fraction of RAM, which eliminates userspace memory.
What actually stops the ladder is **connection identity and descriptor accounting**: the 4-tuple
space first (`EADDRNOTAVAIL` at the portrange ceiling), then the per-process and system-wide file
limits (`EMFILE`/`ENFILE`), then `kern.ipc.maxsockets`. Quote the exact errno and the counter that
was at its limit.

**Q3 — How does your I/O design contribute to that bottleneck?** The Exchange Server is a
single-threaded non-blocking `kqueue` event loop: one thread, one `kqueue` descriptor, and per
connection one socket descriptor, one `Conn` object (a `LineBuffer`, an `OutBuffer`, a role, two
subscription flags — on the order of 150–200 bytes plus the `unordered_map` node) and two `knote`
registrations. There is no per-connection thread, stack, or scheduler entity, so the design
contributes *nothing* to the bottleneck that stopped the ladder: what stopped it was the kernel's
socket and descriptor accounting, which any design must pay. The design's own cost scales with the
number of *ready* connections per wakeup, not with the number registered — an idle connection costs
one `knote` and zero syscalls.

**Q4 — Would changing the I/O/concurrency mechanism help?** No, and the comparison proves the
direction is the other way. Moving from `kqueue` to thread-per-connection would *lower* the ceiling
sharply: `pthread_create` fails at `kern.threads.max_threads_per_proc` (default 1,500 on this
system — cite your measured value), each thread reserves a stack in virtual address space, and each
also carries kernel thread state — so the design fails at roughly 2% of the target while the
descriptor and PCB costs remain unchanged. Moving from `kqueue` to `poll()` would keep the same
memory profile but degrade CPU, because `poll()` passes the entire descriptor array across the
syscall boundary and the kernel scans it in O(n) on every call, whereas `kqueue` registers interest
once and returns only the ready set in O(ready). At 70,000 mostly-idle descriptors that is the
difference between scanning 70,000 entries per wakeup and being handed the one that matters.
`select()` is worse still and is capped by `FD_SETSIZE` (1024 by default) long before any of this
matters.

**Q5 — Quantify the trade-off.** Use the §B.6 table. Report, at a count all three designs reach:
RSS and VSZ per connection for each design, `nlwp`, idle CPU, and the count at which each stopped.
Then report the effect of the single configuration change you made (64 KB thread stacks): it reduces
VSZ per connection by the stack delta but does **not** move `kern.threads.max_threads_per_proc`, so
the ceiling is unchanged — which is the real lesson. Contrast with the configuration changes that
*did* move the kqueue ceiling (`portrange`, `lo0` aliases, `kern.maxfiles`,
`kern.maxfilesperproc`, `kern.ipc.maxsockets`), each with the before/after count.

---

## B.8 Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `connect(): EADDRNOTAVAIL` (errno 49) | ephemeral port range exhausted for that source address | widen `net.inet.ip.portrange`, add `lo0` aliases, pass `--src` |
| `socket(): EMFILE` (errno 24) | per-process descriptor limit | `ulimit -n 200000`, raise `kern.maxfilesperproc` |
| `socket(): ENFILE` (errno 23) | system file table full | raise `kern.maxfiles` |
| `connect(): ECONNREFUSED` mid-run | listen backlog overflowed under the connect burst | `listen(fd, 4096)` patch + `kern.ipc.somaxconn=4096`, or lower `--batch` |
| generator stalls with "connects still pending" | accept queue saturated | lower `--batch` to 64 |
| `sysctl kern.ipc.maxsockets` refuses to set | it is `RDTUN` | put it in `/boot/loader.conf` and reboot |
| server RSS climbs far beyond ~2 KB/conn | output buffers accumulating | confirm the generator sends nothing; idle conns must never enter `queue_out` |
| `netstat` count is exactly double | both endpoints are local | filter on `$4 ~ /\.5000$/` as `measure.sh` does |

## B.9 Add to `README.md`

```
## Bonus (§6.9)
tools/connflood.cpp       client-generation program: creates and holds N idle TCP connections
tools/threaded_server.cpp comparison artifact ONLY (thread-per-connection); not part of the
                          graded Exchange Server, which is single-threaded kqueue
tools/measure.sh          captures one row of the resource table
Build: make bonus
Run:   ./connflood 127.0.0.1 5000 70000 --src 127.0.0.1,127.0.0.2,127.0.0.3
Required tuning is documented in report.pdf §Bonus (loader.conf, sysctls, lo0 aliases, ulimit).
```
