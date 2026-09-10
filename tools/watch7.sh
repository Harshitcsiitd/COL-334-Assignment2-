#!/bin/sh
# Waits for the Exchange Server to appear, pins its PID, samples until it dies.
LOG=${1:-/tmp/exp7_t4.log}

echo "waiting for exchange_server..."
while :; do
    SPID=$(pgrep -n exchange_server) && [ -n "$SPID" ] && break
    sleep 0.2
done
echo "$SPID" > /tmp/server.pid
echo "pinned server pid=$SPID"

while kill -0 "$SPID" 2>/dev/null; do
    echo "================ $(date '+%H:%M:%S')  pid=$SPID ================"
    ps -o pid=,rss=,vsz=,%cpu=,%mem=,nlwp=,state=,wchan= -p "$SPID"
    procstat -k "$SPID" 2>/dev/null | tail -n +2
    echo "--- connections on :5000 ---"
    netstat -an -p tcp | awk 'NR<=2 || $4 ~ /\.5000$/ || $5 ~ /\.5000$/'
    echo "--- socket buffers / timers ---"
    netstat -an -p tcp -x 2>/dev/null | awk 'NR<=2 || /\.5000/'
    sleep 2
done

echo "server pid=$SPID exited at $(date '+%H:%M:%S')"