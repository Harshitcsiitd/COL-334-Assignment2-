#!/bin/sh
# tools/measure.sh — capture one row of the Bonus resource table.
#
#   usage: tools/measure.sh <label>            e.g. tools/measure.sh 40000
#
# Emits a human-readable block to stdout (screenshot this) and appends one CSV
# row to /tmp/bonus.csv (paste this into the report table).
#
# Run it only while the requested number of connections is established and idle.

LABEL=${1:-unlabelled}
CSV=/tmp/bonus.csv
PID=$(pgrep -n exchange_server)
GPID=$(pgrep -n connflood)

if [ -z "$PID" ]; then echo "exchange_server is not running"; exit 1; fi

echo "======================================================================"
echo " BONUS MEASUREMENT   label=$LABEL   $(date '+%Y-%m-%d %H:%M:%S')"
echo " server pid=$PID   generator pid=${GPID:-none}"
echo "======================================================================"

# --- established connections -----------------------------------------------
# Each loopback connection appears TWICE in netstat (both endpoints are local).
# Count only the rows whose LOCAL endpoint is the server's listening port.
EST=$(netstat -an -p tcp | awk '$6=="ESTABLISHED" && $4 ~ /\.5000$/ {n++} END{print n+0}')
PCB=$(sysctl -n net.inet.tcp.pcbcount 2>/dev/null)
echo "-- connections --"
echo "established to server port 5000 : $EST"
echo "net.inet.tcp.pcbcount (all PCBs): ${PCB:-n/a}   # ~2x EST on loopback"

# --- server memory ----------------------------------------------------------
echo "-- server memory / cpu --"
ps -o pid=,rss=,vsz=,%cpu=,%mem=,time=,nlwp= -p "$PID" | \
  awk '{printf "pid=%s rss=%s KB vsz=%s KB cpu=%s%% mem=%s%% time=%s threads=%s\n",$1,$2,$3,$4,$5,$6,$7}'

# --- kernel memory attributable to sockets ---------------------------------
echo "-- kernel UMA zones (ITEM / USED / SIZE) --"
vmstat -z | head -1
vmstat -z | egrep '^(socket|tcpcb|tcp_inpcb|tcp_log|knote|kqueue|mbuf|mbuf_cluster)' 
echo "   (kernel bytes for a zone = USED x SIZE; subtract your idle baseline)"

# --- file descriptors -------------------------------------------------------
echo "-- file descriptors --"
SFD=$(procstat -f "$PID" 2>/dev/null | grep -c 'TCP')
echo "server TCP descriptors          : $SFD"
echo "server total descriptors        : $(procstat -f "$PID" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')"
if [ -n "$GPID" ]; then
  echo "generator TCP descriptors       : $(procstat -f "$GPID" 2>/dev/null | grep -c 'TCP')"
fi
OPEN=$(sysctl -n kern.openfiles); MAXF=$(sysctl -n kern.maxfiles)
echo "system-wide kern.openfiles      : $OPEN / kern.maxfiles=$MAXF"
echo "kern.maxfilesperproc            : $(sysctl -n kern.maxfilesperproc)"
echo "shell RLIMIT_NOFILE (ulimit -n) : $(ulimit -n)"

# --- socket buffers ---------------------------------------------------------
echo "-- socket buffers / mbufs --"
netstat -m | egrep 'mbufs in use|clusters in use|bytes allocated|denied|delayed'
echo "kern.ipc.maxsockbuf             : $(sysctl -n kern.ipc.maxsockbuf)"
echo "kern.ipc.maxsockets             : $(sysctl -n kern.ipc.maxsockets 2>/dev/null)"
echo "net.inet.tcp.sendspace/recvspace: $(sysctl -n net.inet.tcp.sendspace)/$(sysctl -n net.inet.tcp.recvspace)"

# --- system memory ----------------------------------------------------------
echo "-- system memory --"
vmstat -h 1 2 | tail -1
sysctl -n hw.physmem vm.stats.vm.v_free_count | tr '\n' ' '; echo

# --- CSV row ----------------------------------------------------------------
RSS=$(ps -o rss= -p "$PID" | tr -d ' ')
VSZ=$(ps -o vsz= -p "$PID" | tr -d ' ')
CPU=$(ps -o %cpu= -p "$PID" | tr -d ' ')
CLUS=$(netstat -m | awk '/clusters in use/{print $1}')
[ -f "$CSV" ] || echo "label,established,server_rss_kb,server_vsz_kb,server_cpu_pct,server_fds,kern_openfiles,kern_maxfiles,mbuf_clusters,pcbcount" > "$CSV"
echo "$LABEL,$EST,$RSS,$VSZ,$CPU,$SFD,$OPEN,$MAXF,$CLUS,${PCB:-}" >> "$CSV"
echo
echo "CSV row appended to $CSV:"
tail -1 "$CSV"
