#!/bin/bash
# Round-robin hardware runs of several AV player ELFs, then a summary table.
#   tools/run_set.sh ROUNDS A.elf B.elf C.elf ...
# Order is A B C A B C ... (interleaved); never run under `timeout`.
# Logs: set_logs/<elf name>_<n>.log.  Summary: decode avg, sec19, sec24, drops, per-ELF mean.
ROUNDS=${1:?usage: $0 ROUNDS ELF [ELF...]}; shift
[ $# -ge 1 ] || { echo "need at least one ELF"; exit 2; }
IP=${IP:-192.168.0.128}
source /opt/toolchains/dc/kos/environ.sh
set -u
mkdir -p set_logs
for i in $(seq 1 "$ROUNDS"); do
  for f in "$@"; do
    l=$(basename "$f" .elf)
    echo "=== round $i: $l"
    kos-tool -t "$IP" -x "$f" -m . > "set_logs/${l}_$i.log" 2>&1
    echo "    exit $?"
  done
done
echo
printf '%-24s %10s %9s %9s %s\n' run decode_avg sec19 sec24 presented/dropped
for f in "$@"; do
  l=$(basename "$f" .elf); sum=0; n=0
  for i in $(seq 1 "$ROUNDS"); do
    g="set_logs/${l}_$i.log"
    avg=$(sed -n 's/.*video decode total=.* avg=\([0-9.]*\) ms\/frame.*/\1/p' "$g" | head -1)
    s19=$(sed -n 's/.*sec 19: dec= *[0-9.]* ([0-9 ]*fr, avg *\([0-9.]*\),.*/\1/p' "$g" | head -1)
    s24=$(sed -n 's/.*sec 24: dec= *[0-9.]* ([0-9 ]*fr, avg *\([0-9.]*\),.*/\1/p' "$g" | head -1)
    pd=$(sed -n 's/.*video decoded=[0-9]* presented=\([0-9]*\) dropped=\([0-9]*\).*/\1\/\2/p' "$g" | head -1)
    printf '%-24s %10s %9s %9s %s\n' "${l}_$i" "${avg:-?}" "${s19:-?}" "${s24:-?}" "${pd:-?}"
    [ -z "$avg" ] && echo "    no result; log says: $(grep -m1 -i 'fail\|unable\|error' "$g" || tail -1 "$g")"
    [ -n "$avg" ] && { sum=$(echo "$sum + $avg" | bc -l); n=$((n+1)); }
  done
  [ "$n" -gt 0 ] && printf '%-24s %10.3f\n' "  mean $l" "$(echo "$sum / $n" | bc -l)"
done
