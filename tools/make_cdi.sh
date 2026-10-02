#!/bin/bash
# Build one Flycast/console-bootable CDI per AV player ELF.
#   tools/make_cdi.sh ref_plain_stripped.elf float_stripped.elf dc-libavmpeg-av-mac.elf
# The clip named by fixture= in config.ini (default lair_320_23976_30s_spec.mpg)
# and config.ini itself go on the disc root: the player tries /cd/ first, then /pc/.
# A CDI boots without dcload, so /pc/ does not exist on it.
# Output: build_cdi/<elf basename>.cdi
set -eu
[ $# -ge 1 ] || { echo "usage: $0 ELF [ELF...]"; exit 2; }
source /opt/toolchains/dc/kos/environ.sh
CFG=config.ini
FIX=$(sed -n 's/^fixture=\(.*\)$/\1/p' "$CFG" | head -1)
FIX=${FIX:-lair_320_23976_30s_spec.mpg}
[ -f "fixtures/$FIX" ] || { echo "missing fixtures/$FIX"; exit 1; }
mkdir -p build_cdi
for elf in "$@"; do
  name=$(basename "$elf" .elf)
  out="build_cdi/$name.cdi"
  echo "== $out  (clip: $FIX)"
  # one disc, clip + config.ini as root files; fall back to clip only if mkdcdisc
  # rejects a second -f
  mkdcdisc -e "$elf" -n "$name" -N -f "fixtures/$FIX" -f "$CFG" -o "$out" \
    || { echo "   retrying without config.ini on the disc"; \
         mkdcdisc -e "$elf" -n "$name" -N -f "fixtures/$FIX" -o "$out"; }
done
echo "done: $(ls build_cdi/*.cdi | wc -l) image(s) in build_cdi/"
