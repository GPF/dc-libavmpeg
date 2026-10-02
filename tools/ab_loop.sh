#!/bin/bash
# Alternating A/B hardware runs of two AV player ELFs, then a summary table.
#   tools/ab_loop.sh A.elf B.elf [rounds=3] [dc_ip=192.168.0.128]
# Runs A B A B ... (never under `timeout`; let each run finish on its own).
# Logs go to ab_logs/<label>_<n>.log; the summary shows decode avg, seconds 19
# and 24, and presented/dropped per run plus the per-side mean.
A=${1:?usage: $0 A.elf B.elf [rounds] [dc_ip]}
B=${2:?usage: $0 A.elf B.elf [rounds] [dc_ip]}
N=${3:-3}
IP=${4:-192.168.0.128}
OUT=ab_logs
mkdir -p "$OUT"
# environ_base.sh reads unset variables, so source it before enabling set -u
source /opt/toolchains/dc/kos/environ.sh
set -u

label() { basename "$1" .elf; }
LA=$(label "$A"); LB=$(label "$B")

for i in $(seq 1 "$N"); do
  for pair in "$LA:$A" "$LB:$B"; do
    l=${pair%%:*}; f=${pair#*:}
    echo "=== round $i: $l"
    kos-tool -t "$IP" -x "$f" -m . > "$OUT/${l}_$i.log" 2>&1
    echo "    exit $?"
  done
done

echo
printf '%-28s %10s %9s %9s %s\n' run decode_avg sec19 sec24 presented/dropped
for l in "$LA" "$LB"; do
  sum=0; n=0
  for i in $(seq 1 "$N"); do
    f="$OUT/${l}_$i.log"
    avg=$(sed -n 's/.*video decode total=.* avg=\([0-9.]*\) ms\/frame.*/\1/p' "$f" | head -1)
    s19=$(sed -n 's/.*sec 19: dec= *[0-9.]* ([0-9 ]*fr, avg *\([0-9.]*\),.*/\1/p' "$f" | head -1)
    s24=$(sed -n 's/.*sec 24: dec= *[0-9.]* ([0-9 ]*fr, avg *\([0-9.]*\),.*/\1/p' "$f" | head -1)
    pd=$(sed -n 's/.*video decoded=[0-9]* presented=\([0-9]*\) dropped=\([0-9]*\).*/\1\/\2/p' "$f" | head -1)
    printf '%-28s %10s %9s %9s %s\n' "${l}_$i" "${avg:-?}" "${s19:-?}" "${s24:-?}" "${pd:-?}"
    if [ -z "$avg" ]; then
      echo "    no result; log says: $(grep -m1 -i 'fail\|unable\|error' "$f" || tail -1 "$f")"
    fi
    if [ -n "$avg" ]; then sum=$(echo "$sum + $avg" | bc -l); n=$((n+1)); fi
  done
  [ "$n" -gt 0 ] && printf '%-28s %10.3f\n' "  mean $l" "$(echo "$sum / $n" | bc -l)"
done
