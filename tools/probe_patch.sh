#!/bin/bash
# usage: probe_patch.sh <in.elf> <event-hex e.g. 25> <out.elf>
# Injects the layout-neutral whole-decode event probe (src/probe_patch.c) into a
# linked production ELF by adding a section in the .text/.init gap and patching
# the single literal-pool word in _decode_video that holds &avcodec_decode_video.
# Requires: source /opt/toolchains/dc/kos/environ.sh
set -e
IN="$1"; EV="$2"; OUT="$3"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"
nm_addr() { sh-elf-nm "$IN" | awk -v s="$1" '$3==s{print $1; exit}'; }
A=$(nm_addr _avcodec_decode_video); P=$(nm_addr _printf); D=$(nm_addr _decode_video)
[ -n "$A" ] && [ -n "$P" ] && [ -n "$D" ] || { echo "missing symbols in $IN"; exit 1; }
kos-cc -O2 -fno-lto -ffat-lto-objects -fno-builtin-printf -I"$HERE/src" -I"$HERE/vendor/ffmpeg" \
    -I"$HERE/vendor/ffmpeg/libavcodec" -I"$HERE/vendor/ffmpeg/libavutil" -c "$HERE/src/probe_patch.c" -o "$W/b.o"
sh-elf-ld -m shlelf -T "$HERE/tools/probe_patch.ld" -o "$W/b.elf" "$W/b.o" \
    --defsym=_probe_real_decode=0x$A --defsym=_printf=0x$P --defsym=_CAL_EVENT=0x$EV
sh-elf-objcopy -O binary "$W/b.elf" "$W/b.bin"
ENTRY=$(sh-elf-nm "$W/b.elf" | awk '$3=="_probe_wrap_decode"{print $1}')
SIZE=$(stat -c%s "$W/b.bin")
# end of .text and start of .init must bracket the blob
TEXT_END=$(sh-elf-size -A "$IN" | awk '$1==".text"{printf "%d", $3+$2}')
INIT=$(sh-elf-size -A "$IN" | awk '$1==".init"{printf "%d", $3}')
BASE=$((0x8c10fa00))
[ $TEXT_END -le $BASE ] && [ $((BASE+SIZE)) -le $INIT ] || { echo "blob does not fit: text_end=$TEXT_END blob=$BASE+$SIZE init=$INIT"; exit 1; }
# locate the literal word holding &avcodec_decode_video inside _decode_video
LIT=$(sh-elf-objdump -d --no-show-raw-insn "$IN" --start-address=0x$D --stop-address=$((0x$D+0x300)) \
    | awk -v a="$A" '$0 ~ ("! " a " <_avcodec_decode_video>") {print $3; exit}')
[ -n "$LIT" ] || { echo "no literal for avcodec_decode_video found in _decode_video"; exit 1; }
TOFF=$(sh-elf-readelf -S -W "$IN" | awk '/\] \.text /{print $6}')
TADDR=$(sh-elf-nm "$IN" | awk '$3=="_start"{print $1; exit}')
sh-elf-objcopy --add-section .instr="$W/b.bin" --set-section-flags .instr=alloc,load,readonly,code,contents \
    --change-section-address .instr=$BASE "$IN" "$W/patched.elf"
# re-read .text offset in the new file and patch the literal word
TOFF=$(sh-elf-readelf -S -W "$W/patched.elf" | awk '/\] \.text /{print $6}')
python3 - "$W/patched.elf" "$OUT" $((16#$TOFF)) $((16#$LIT)) $((16#$ENTRY)) $((16#$A)) <<'PY'
import sys,struct
src,dst,toff,lit,entry,old=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5]),int(sys.argv[6])
d=bytearray(open(src,'rb').read())
o=toff+(lit-0x8c010000)
cur=struct.unpack('<I',d[o:o+4])[0]
assert cur==old, "literal at %08x is %08x, expected %08x"%(lit,cur,old)
d[o:o+4]=struct.pack('<I',entry)
open(dst,'wb').write(d)
print("patched literal %08x: %08x -> %08x"%(lit,old,entry))
PY
echo "blob $SIZE bytes at $BASE, entry 0x$ENTRY, real decode 0x$A, printf 0x$P"
rm -rf "$W"
