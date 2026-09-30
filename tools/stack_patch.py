#!/usr/bin/env python3
"""Move the initial (main-thread) stack top of a linked KOS ELF by patching the
two data words the startup stub loads it from (_arch_stack_16m / _arch_stack_32m).
Layout-neutral: no instruction or symbol moves; only 8 bytes of .data change.
usage: stack_patch.py in.elf out.elf DELTA   (DELTA bytes subtracted from the stack top)
Requires sh-elf-nm/sh-elf-readelf on PATH (source KOS environ.sh)."""
import re, struct, subprocess, sys

src, dst, delta = sys.argv[1], sys.argv[2], int(sys.argv[3])
nm = subprocess.run(["sh-elf-nm", src], capture_output=True, text=True).stdout
rd = subprocess.run(["sh-elf-readelf", "-S", "-W", src], capture_output=True, text=True).stdout
m = re.search(r"\] \.data\s+PROGBITS\s+([0-9a-f]+) ([0-9a-f]+) ", rd)
daddr, doff = int(m.group(1), 16), int(m.group(2), 16)
d = bytearray(open(src, "rb").read())
for sym in ("_arch_stack_16m", "_arch_stack_32m"):
    a = int(re.search(r"^([0-9a-f]+) . " + sym + "$", nm, re.M).group(1), 16)
    o = doff + (a - daddr)
    v = struct.unpack("<I", d[o:o + 4])[0]
    assert v in (0x8d000000, 0x8e000000), hex(v)
    d[o:o + 4] = struct.pack("<I", v - delta)
open(dst, "wb").write(d)
print("stack top shifted by -%d in %s" % (delta, dst))
