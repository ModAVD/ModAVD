#!/system/bin/sh
# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# ModAVD#96 download window, run as root in the guest.
# usage: dl-measure.sh <label> <secs> <warmup> <wait_for_download_s>
# Waits for the first *.downloading (the game's resource download has started), skips <warmup> s,
# then for <secs> s samples every 10 s: uptime, rx bytes (all non-lo interfaces), /data's block
# devices from /proc/diskstats, the number of *.downloading files; du -sk of files/ at the start,
# every 30 s and at the end (du growth over the window = the metric used for #35 / #96 so far).
# SurfaceFlinger timestats (the game's frame pacing) are cleared at the window start and dumped
# every 30 s into $OUT/ts-<i>.txt. At the end: the game's pid, Seccomp_filters, ModAvdFsync lines.
P=com.hypergryph.arknights
F=/data/media/0/Android/data/$P/files
L=$1; S=${2:-150}; W=${3:-20}; WT=${4:-240}
OUT=/data/local/tmp/t96/dl-$L
rm -rf "$OUT"; mkdir -p "$OUT"
up() { cut -d' ' -f1 /proc/uptime; }
rx() { awk '/:/ && $1 !~ /^lo:/ { v = $0; sub(/^[^:]*:/, "", v); split(v, a, " "); s += a[1] } END { printf "%d", s }' /proc/net/dev; }
DM=$(basename "$(readlink -f "$(awk '$2 == "/data" { print $1; exit }' /proc/mounts)")")
ds() { awk -v d="$DM" '$3 == "vdc" || $3 == d { printf "%s:w=%s,ws=%s,fl=%s ", $3, $8, $10, $19 }' /proc/diskstats; }
dus() { du -sk "$F" 2>/dev/null | cut -f1; }
cpu() { awk '/^cpu / { printf "%s,%s,%s,%s,%s,%s,%s", $2, $3, $4, $5, $6, $7, $8 } /^procs_blocked/ { printf " blocked=%s", $2 }' /proc/stat; }
ndl() { find "$F" -name '*.downloading' 2>/dev/null | wc -l; }
ts_dump() { dumpsys SurfaceFlinger --timestats -dump -maxlayers 3 > "$OUT/ts-$1.txt" 2>&1; dumpsys SurfaceFlinger --timestats -clear > /dev/null 2>&1; }
echo "label=$L clocksource=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource) uptime=$(up) data_dev=$DM"
echo "props: defer=[$(getprop modavd.fsync.defer)] mode=[$(getprop modavd.fsync.defer.mode)] log=[$(getprop modavd.fsync.log)]"
t=0; first=
while [ $t -lt "$WT" ]; do
    first=$(find "$F/HGGameUpdate/HGDownload" -name '*.downloading' 2>/dev/null | head -1)
    [ -n "$first" ] && break
    sleep 2; t=$((t + 2))
done
if [ -z "$first" ]; then echo "NO DOWNLOAD after ${WT}s"; exit 2; fi
echo "download_started_after=${t}s"
sleep "$W"
dumpsys SurfaceFlinger --timestats -enable > /dev/null 2>&1
dumpsys SurfaceFlinger --timestats -clear > /dev/null 2>&1
k=0; n=$((S / 10))
echo "sample i=0 up=$(up) du_kb=$(dus) rx=$(rx) ds: $(ds) dl_files=$(ndl) cpu=$(cpu)"
while [ $k -lt "$n" ]; do
    sleep 10; k=$((k + 1))
    d=-
    if [ $((k % 3)) -eq 0 ] || [ $k -eq "$n" ]; then d=$(dus); ts_dump $k; fi
    echo "sample i=$k up=$(up) du_kb=$d rx=$(rx) ds: $(ds) dl_files=$(ndl) cpu=$(cpu)"
done
pid=$(pidof $P)
echo "game pid: $pid"
for p in $pid; do echo "pid $p $(grep -E 'Seccomp_filters|NoNewPrivs' /proc/$p/status | tr '\n' ' ')"; done
logcat -d -s ModAvdFsync | grep -E 'armed|summary|stuck|not running|NOTIF' | tail -6
