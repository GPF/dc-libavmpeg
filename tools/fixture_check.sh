#!/bin/bash
# Run reference vs MAC.W IDCT players over several fixtures: frame checksums (first 30
# frames, from the FRAME_TRACE builds) must match, and decode time is compared.
#   tools/fixture_check.sh [-n rounds] [-i dc_ip] fixture.mpg [fixture2.mpg ...]
# Environment (ELF paths, relative to the repo root):
#   REF_TRACE V2_TRACE   FRAME_TRACE builds (default dc-libavmpeg-av-ref-trace.elf, v2_trace_stripped.elf)
#   REF V2               timing builds     (default ref_plain_stripped.elf, v2_player_stripped.elf)
# Run from the repo root (kos-tool -m . exposes it as /pc/). config.ini is edited per
# fixture and restored on exit. Logs: fixture_logs/<fixture>_<label>_<n>.log
N=2; IP=192.168.0.128
while getopts n:i: o; do case $o in n) N=$OPTARG;; i) IP=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
[ $# -ge 1 ] || { sed -n 2,11p "$0"; exit 2; }
REF_TRACE=${REF_TRACE:-dc-libavmpeg-av-ref-trace.elf}
V2_TRACE=${V2_TRACE:-v2_trace_stripped.elf}
REF=${REF:-ref_plain_stripped.elf}
V2=${V2:-v2_player_stripped.elf}
for f in "$REF_TRACE" "$V2_TRACE" "$REF" "$V2" config.ini; do
  [ -e "$f" ] || { echo "missing $f (run from the repo root)"; exit 2; }
done
source /opt/toolchains/dc/kos/environ.sh
set -u
mkdir -p fixture_logs
cp config.ini fixture_logs/config.ini.orig
trap 'cp fixture_logs/config.ini.orig config.ini' EXIT

avg() { sed -n 's/.*video decode total=.* avg=\([0-9.]*\) ms\/frame.*/\1/p' "$1" | head -1; }
run() {   # elf log; retried once if the run produced no result (e.g. a dcload transfer glitch)
  local t
  for t in 1 2; do
    kos-tool -t "$IP" -x "$1" -m . > "$2" 2>&1
    grep -aq 'video decoded=' "$2" && return 0
    [ "$t" = 1 ] && echo "  retrying $1 on $(basename "$2")" >&2
  done
}
pd()  { sed -n 's/.*video decoded=\([0-9]*\) presented=\([0-9]*\) dropped=\([0-9]*\).*/\1 \2\/\3/p' "$1" | head -1; }

printf '%-36s %-9s %-12s %9s %9s %8s\n' fixture checksums dec/pres/drop ref_ms v2_ms delta
for fx in "$@"; do
  fx=$(basename "$fx"); tag=${fx%.*}
  if grep -q '^fixture=' fixture_logs/config.ini.orig; then
    sed "s|^fixture=.*|fixture=$fx|" fixture_logs/config.ini.orig > config.ini
  else
    { cat fixture_logs/config.ini.orig; echo "fixture=$fx"; } > config.ini
  fi
  run "$REF_TRACE" "fixture_logs/${tag}_reftrace.log"
  run "$V2_TRACE"  "fixture_logs/${tag}_v2trace.log"
  grep FRAME_CHECKSUM "fixture_logs/${tag}_reftrace.log" > "fixture_logs/${tag}_ref.sums"
  grep FRAME_CHECKSUM "fixture_logs/${tag}_v2trace.log"  > "fixture_logs/${tag}_v2.sums"
  if [ -s "fixture_logs/${tag}_ref.sums" ] && cmp -s "fixture_logs/${tag}_ref.sums" "fixture_logs/${tag}_v2.sums"; then
    sums="MATCH($(wc -l < fixture_logs/${tag}_ref.sums))"
  elif [ ! -s "fixture_logs/${tag}_ref.sums" ]; then sums="NO-REF"
  else sums="DIFFER"; fi
  rs=0; vs=0; n=0
  for i in $(seq 1 "$N"); do
    run "$REF" "fixture_logs/${tag}_ref_$i.log"; run "$V2" "fixture_logs/${tag}_v2_$i.log"
    a=$(avg "fixture_logs/${tag}_ref_$i.log"); b=$(avg "fixture_logs/${tag}_v2_$i.log")
    [ -n "$a" ] && [ -n "$b" ] && { rs=$(echo "$rs + $a" | bc -l); vs=$(echo "$vs + $b" | bc -l); n=$((n+1)); }
  done
  info=$(for i in $(seq 1 "$N"); do pd "fixture_logs/${tag}_v2_$i.log"; done | head -1 | tr ' ' '/')
  if [ "$n" -gt 0 ]; then
    printf '%-36s %-9s %-12s %9.3f %9.3f %+8.3f\n' "$fx" "$sums" "${info:-?}" \
      "$(echo "$rs / $n" | bc -l)" "$(echo "$vs / $n" | bc -l)" "$(echo "($vs - $rs) / $n" | bc -l)"
  else
    printf '%-36s %-9s %-12s %s\n' "$fx" "$sums" "${info:-?}" "no timing result (see fixture_logs/${tag}_*)"
  fi
done
