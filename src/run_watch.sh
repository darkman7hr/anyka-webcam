#!/bin/sh
# Watch a running akrtsp without changing anything: process CPU in ticks, free
# memory, and who is connected. This is the script that answers the question the
# whole exercise is about - what does our server cost with the real NVR attached,
# against the ~33% the vendor binary costs in the same situation.
#
#   sh /tmp/run_watch.sh                 # watch akrtsp, 12 x 5 s
#   sh /tmp/run_watch.sh 8 rtsp          # watch the vendor binary, same method
#
# CPU is read from /proc/<pid>/stat (utime+stime in ticks) rather than top's %CPU
# column, because top on this box averages over its whole refresh and swings
# wildly; clock ticks over a measured interval do not lie. HZ is 100 here.
export HZ=100
N=${1:-12}
PAT=${2:-akrtsp}

# Match processes by /proc/<pid>/comm, not by grepping ps output. Two real bugs
# this avoids: the vendor binary runs as /mnt/Factory/custom/rtsp from the init
# scripts but as "./rtsp" when restarted by hand (a path grep matches nothing and
# leaves it holding the ISP), and a word grep matches the *watching script's own
# argv* - "sh /tmp/run_watch.sh 8 rtsp" contains the word rtsp, which is how one
# run of this script measured its own grep pipeline and reported cpu=0.
pids_by_comm()
{
	for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
		[ "$(cat /proc/$p/comm 2>/dev/null)" = "$1" ] && echo "$p"
	done
}

ak_pids()
{
	pids_by_comm akrtsp
}

watch_pids()
{
	pids_by_comm "$PAT"
}

ticks()
{
	# utime is field 14, stime field 15 of /proc/<pid>/stat (comm can contain
	# spaces, so cut after the last ')')
	awk '{split($0, a, ") "); split(a[2], f, " "); print f[12] + f[13]}' /proc/$1/stat 2>/dev/null
}

P=$(watch_pids)
[ -n "$P" ] || { echo "no $PAT process running"; exit 1; }
echo "watching pid $P for $((N * 5)) s"

prev=$(ticks "$P")
pt=$(date +%s)
i=0
while [ $i -lt $N ]; do
	sleep 5
	P=$(watch_pids)
	[ -z "$P" ] && { echo "GONE at t=$((i * 5))s"; break; }
	now=$(ticks "$P")
	nt=$(date +%s)
	el=$((nt - pt))
	[ $el -le 0 ] && el=1
	# percent of one core: ticks/HZ = seconds of CPU over el seconds
	pct=$(( (now - prev) * 100 / (HZ * el) ))
	echo "t=$((i * 5))s pid=$P cpu=${pct}% of one core  $(free | grep Mem | awk '{print "mem free " $4 " KB"}')"
	prev=$now
	pt=$nt
	i=$((i + 1))
done

echo "-- sessions on :554 --"
netstat -tan 2>/dev/null | grep ':554' | head -8
if [ "$PAT" = "akrtsp" ]; then
	echo "-- log tail --"
	tail -6 /tmp/akrtsp.log 2>/dev/null
fi
