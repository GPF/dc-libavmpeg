#!/usr/bin/env python3
"""Stackless per-region event probe, injected into a linked KOS ELF after the fact.

usage: probe2.py in.elf EVENT_HEX out.elf      (source /opt/toolchains/dc/kos/environ.sh first)

Regions measured with one PRFC1 event (PRFC0 stays KOS's timer):
  dec   whole avcodec_decode_video() call, split by returned picture type
  mb    MPV_decode_mb()   (macroblock reconstruction, includes idct and mc)
  idct  ff_simple_idct_add / ff_simple_idct_put   (called through dsp function pointers)
  mc    the 16 SH-4 put/avg pixel routines installed in the dsp tables

Why this shape: code placement alone moves decode time by up to 12% (docs/STATUS.md)
and the stack is the hot partner, so the probe must not (a) move any existing code or
symbol, nor (b) push a stack frame under the code it measures. Each wrapper therefore
keeps the caller's pr in a global, points pr at a small return stub and tail-jumps to
the real function; no stack is used. Only 4-byte literal-pool words holding function
addresses are changed in the original image. The blob lives at 0x8c10f880, in the gap
between the end of .text and .init. Wrappers are not re-entrant per region, which is
fine here (none of these functions recurse or nest within their own region).
"""
import re, struct, subprocess, sys, os, tempfile

BASE = None   # chosen below: end of .text + 0x70, 32-byte aligned (0x8c10f880 for the production av ELF)
inp, ev, out = sys.argv[1], sys.argv[2], sys.argv[3]
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def run(*a, **k):
    return subprocess.run(a, check=True, capture_output=True, text=True, **k).stdout

nm = run("sh-elf-nm", "-S", "-n", inp).splitlines()
syms = {}      # name -> (addr, size)
ranges = []    # (addr, end, name) for text symbols
for l in nm:
    p = l.split()
    if len(p) == 4 and p[2] in "Tt":
        a, s = int(p[0], 16), int(p[1], 16)
        syms[p[3]] = (a, s)
        ranges.append((a, a + s, p[3]))
    elif len(p) == 3 and p[1] in "Tt":
        syms.setdefault(p[2], (int(p[0], 16), 0))

def owner(addr):
    for a, e, n in ranges:
        if a <= addr < e:
            return n
    return None

sec = run("sh-elf-readelf", "-S", "-W", inp)
m = re.search(r"\] \.text\s+PROGBITS\s+([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)", sec)
taddr, toff, tsize = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
data = open(inp, "rb").read()
text = data[toff:toff + tsize]

def literal_words(value):
    """4-aligned addresses in .text whose 32-bit word equals value."""
    b = struct.pack("<I", value)
    res, i = [], text.find(b)
    while i != -1:
        if (taddr + i) % 4 == 0:
            res.append(taddr + i)
        i = text.find(b, i + 1)
    return res

def find_literal(fn_name, scope_re):
    """literal word holding &fn_name inside a function whose name matches scope_re"""
    a = syms[fn_name][0]
    hits = [w for w in literal_words(a) if owner(w) and re.search(scope_re, owner(w))]
    if len(hits) != 1:
        raise SystemExit("expected exactly one literal for %s in %s, got %s (all: %s)" %
                         (fn_name, scope_re, [hex(h) for h in hits], [hex(w) for w in literal_words(a)]))
    return hits[0]

def find_literals_excluding(fn_name, exclude_re):
    """all literal words holding &fn_name whose owning function does not match exclude_re"""
    a = syms[fn_name][0]
    return [w for w in literal_words(a) if owner(w) and not re.search(exclude_re, owner(w))]

targets = []   # (region, tag, real_name, literal_addr)
# decode call sites: the player's own (av: _decode_video; probe: _main.constprop.0; default: its
# decode helper). Exclude stream-probing code in libavformat and the codec's own internals.
DEC_EXCL = r"find_stream_info|avformat|open_input|^_avcodec_"
dec_fn = next((n for n in ("_avcodec_decode_video", "_avcodec_decode_video2") if n in syms and
               find_literals_excluding(n, DEC_EXCL)), None)
data_targets = []   # (addr) of 4-byte data words to redirect when the decode call is an indirect codec call
if dec_fn:
    for w in find_literals_excluding(dec_fn, DEC_EXCL):
        targets.append(("dec", "dec", dec_fn, w))
else:
    # LTO inlined avcodec_decode_video: the decode goes through AVCodec.decode, a pointer in .data
    dec_fn = "_mpeg_decode_frame"
    secs = {}
    for mm in re.finditer(r"\] (\.\w+)\s+PROGBITS\s+([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)", sec):
        secs[mm.group(1)] = (int(mm.group(2), 16), int(mm.group(3), 16), int(mm.group(4), 16))
    bpat = struct.pack("<I", syms[dec_fn][0])
    for sn in (".data", ".rodata"):
        if sn in secs:
            sa, so, ss = secs[sn]; blob_ = data[so:so + ss]; i = blob_.find(bpat)
            while i != -1:
                if (sa + i) % 4 == 0: data_targets.append((sn, sa + i, so + i))
                i = blob_.find(bpat, i + 1)
    assert len(data_targets) == 1, data_targets
mb_hits = [w for w in literal_words(syms["_MPV_decode_mb"][0])
           if owner(w) and re.match(r"^_mpeg_decode_slice(\.lto_priv\.\d+)?$", owner(w))]
assert len(mb_hits) == 1, mb_hits
targets.append(("mb", "mb", "_MPV_decode_mb", mb_hits[0]))
# PROBE_IDCT_SUFFIX=_mac probes the MPEG_IDCT_ASM=2 kernels (ff_simple_idct_{add,put}_mac)
IDCT_SUFFIX = os.environ.get("PROBE_IDCT_SUFFIX", "")
for nm_ in ("_ff_simple_idct_add" + IDCT_SUFFIX, "_ff_simple_idct_put" + IDCT_SUFFIX):
    targets.append(("idct", nm_[1:], nm_, find_literal(nm_, r"^_MPV_common_init$")))
mc_names = sorted(n for n in syms if re.match(r"_(put|avg)_rnd_pixels(8|16)_(o|x|y|xy)\.lto_priv\.\d+$", n))
assert len(mc_names) == 16, mc_names
for n in mc_names:
    targets.append(("mc", n[1:].replace(".", "_"), n, find_literal(n, r"^_MPV_common_init$")))

# offsetof(AVFrame, pict_type): compile a const, read its bytes at the symbol's offset
W = tempfile.mkdtemp()
open(W + "/o.c", "w").write('#include <stddef.h>\n#include "libavcodec/avcodec.h"\n'
                            'const unsigned cal_pt_off = offsetof(AVFrame, pict_type);\n')
run("kos-cc", "-O0", "-fno-lto", "-fno-data-sections", "-fno-function-sections",
    "-I" + HERE + "/vendor/ffmpeg", "-I" + HERE + "/vendor/ffmpeg/libavcodec",
    "-I" + HERE + "/vendor/ffmpeg/libavutil", "-c", W + "/o.c", "-o", W + "/o.o")
_nm = run("sh-elf-nm", W + "/o.o")
_sym = re.search(r"^([0-9a-f]+) [RrDd] _cal_pt_off$", _nm, re.M)
_off = int(_sym.group(1), 16)
_sect = ".rodata" if re.search(r"^[0-9a-f]+ [Rr] _cal_pt_off$", _nm, re.M) else ".data"
run("sh-elf-objcopy", "-O", "binary", "-j", _sect, W + "/o.o", W + "/o.bin")
PT_OFF = struct.unpack("<I", open(W + "/o.bin", "rb").read()[_off:_off + 4])[0]
assert 0 < PT_OFF < 512 and PT_OFF % 4 == 0, PT_OFF

tend0 = taddr + tsize
BASE = (tend0 + 0x70 + 31) & ~31
printf = syms["_printf"][0]
real = {t[2]: syms[t[2]][0] for t in targets}
real['_avcodec_decode_video'] = syms[dec_fn][0]

S = []
w = S.append
w("\t.text\n\t.align 2\n")
# ---- decode wrapper -------------------------------------------------------
w(f"""
\t.globl _probe_entry_dec
_probe_entry_dec:
\tmov.l\t.Ldstate, r0
\tmov.l\t@r0, r1
\ttst\tr1, r1
\tbf\t1f
\tmov\t#1, r1
\tmov.l\tr1, @r0
\tmov.l\t.Lpmcr, r2
\tmov.w\t@r2, r1
\tmov.l\t.Lmask, r3
\tand\tr3, r1
\tmov.l\t.Lclr, r3
\tor\tr3, r1
\tmov.w\tr1, @r2
\tmov.l\t.Lrunev, r1
\tmov.w\tr1, @r2
1:
\tsts\tpr, r1
\tmov.l\tr1, @(4,r0)
\tmov.l\tr5, @(8,r0)
\tmov.l\tr6, @(12,r0)
\tmov.l\t.Lpmc, r1
\tmov.l\t@r1, r2
\tmov.l\tr2, @(16,r0)
\tmov.l\t.Lretdec, r1
\tlds\tr1, pr
\tmov.l\t.Lrealdec, r1
\tjmp\t@r1
\t nop
\t.align 2
.Ldstate:\t.long _probe_dstate
.Lpmcr:\t.long 0xff000088
.Lmask:\t.long 0xffff3fff
.Lclr:\t.long 0x2000
.Lrunev:\t.long 0xc000+0x{ev}
.Lpmc:\t.long 0xff100010
.Lretdec:\t.long _probe_ret_dec
.Lrealdec:\t.long 0x{syms[dec_fn][0]:08x}

_probe_ret_dec:
\tmov.l\t.Ldstate2, r1
\tmov.l\tr0, @(24,r1)
\tmov.l\t.Lpmc2, r2
\tmov.l\t@r2, r2
\tmov.l\t@(16,r1), r3
\tsub\tr3, r2
\tmov\t#0, r4
\tmov.l\t@(12,r1), r3
\tmov.l\t@r3, r3
\ttst\tr3, r3
\tbt\t2f
\tmov.l\t@(8,r1), r3
\tmov.l\t.Lptoff, r5
\tadd\tr5, r3
\tmov.l\t@r3, r4
\tmov\tr4, r5
\tadd\t#-1, r5
\tmov\t#2, r6
\tcmp/hi\tr6, r5
\tbf\t2f
\tmov\t#0, r4
2:
\tshll2\tr4
\tmov.l\t.Laccdec, r3
\tadd\tr4, r3
\tmov.l\t@r3, r5
\tadd\tr2, r5
\tmov.l\tr5, @r3
\tmov.l\t.Lndec, r3
\tadd\tr4, r3
\tmov.l\t@r3, r5
\tadd\t#1, r5
\tmov.l\tr5, @r3
\tmov.l\t@(20,r1), r3
\tadd\t#-1, r3
\ttst\tr3, r3
\tbf\t3f
\tmov\t#120, r3
\tmov.l\tr3, @(20,r1)
\tmov.l\t.Lprint, r0
\tjsr\t@r0
\t nop
\tmov.l\t.Ldstate2, r1
\tbra\t4f
\t nop
3:
\tmov.l\tr3, @(20,r1)
4:
\tmov.l\t@(4,r1), r2
\tlds\tr2, pr
\trts
\t mov.l\t@(24,r1), r0
\t.align 2
.Ldstate2:\t.long _probe_dstate
.Lpmc2:\t.long 0xff100010
.Lptoff:\t.long {PT_OFF}
.Laccdec:\t.long _probe_acc_dec
.Lndec:\t.long _probe_n_dec
.Lprint:\t.long _probe_print
""")
# ---- region machinery -----------------------------------------------------
for R in ("mb", "idct", "mc"):
    w(f"""
\t.align 2
_probe_common_{R}:
\tsts\tpr, r2
\tmov.l\tr2, @r0
\tmov.l\t.Lpmc_{R}, r2
\tmov.l\t@r2, r2
\tmov.l\tr2, @(4,r0)
\tmov.l\tr15, @(12,r0)
\tmov.l\t@(8,r0), r2
\tlds\tr2, pr
\tjmp\t@r1
\t nop
\t.align 2
.Lpmc_{R}:\t.long 0xff100010

_probe_ret_{R}:
\tmov.l\t.Lpmcx_{R}, r1
\tmov.l\t@r1, r2
\tmov.l\t.Ldesc_{R}, r1
\tmov.l\t@(4,r1), r3
\tsub\tr3, r2
\tmov.l\t.Lacc_{R}, r3
\tmov.l\t@r3, r0
\tadd\tr2, r0
\tmov.l\tr0, @r3
\tmov.l\t.Ln_{R}, r3
\tmov.l\t@r3, r0
\tadd\t#1, r0
\tmov.l\tr0, @r3
\tmov.l\t@r1, r2
\tlds\tr2, pr
\trts
\t nop
\t.align 2
.Lpmcx_{R}:\t.long 0xff100010
.Ldesc_{R}:\t.long _probe_desc_{R}
.Lacc_{R}:\t.long _probe_acc_{R}
.Ln_{R}:\t.long _probe_n_{R}
""")
# ---- per-function entries -------------------------------------------------
entries = {}
for R, tag, rn, lit in targets:
    if R == "dec":
        continue
    lbl = f"_probe_entry_{tag}"
    entries[(R, rn)] = lbl
    w(f"""
\t.align 2
\t.globl {lbl}
{lbl}:
\tmov.l\t1f, r1
\tmov.l\t2f, r0
\tbra\t_probe_common_{R}
\t nop
\t.align 2
1:\t.long 0x{real[rn]:08x}
2:\t.long _probe_desc_{R}
""")
w("""
\t.data
\t.align 2
\t.globl _probe_acc_dec
\t.globl _probe_n_dec
\t.globl _probe_acc_mb
\t.globl _probe_n_mb
\t.globl _probe_acc_idct
\t.globl _probe_n_idct
\t.globl _probe_acc_mc
\t.globl _probe_n_mc
_probe_acc_dec:\t.long 0,0,0,0
_probe_n_dec:\t.long 0,0,0,0
_probe_acc_mb:\t.long 0
_probe_n_mb:\t.long 0
_probe_acc_idct:\t.long 0
_probe_n_idct:\t.long 0
_probe_acc_mc:\t.long 0
_probe_n_mc:\t.long 0
_probe_dstate:\t.long 0,0,0,0,0,120,0
""")
for R in ("mb", "idct", "mc"):
    w(f"\t.globl _probe_desc_{R}\n_probe_desc_{R}:\t.long 0,0,_probe_ret_{R},0\n")

open(W + "/p.S", "w").write("".join(S))
run("kos-cc", "-c", "-x", "assembler-with-cpp", W + "/p.S", "-o", W + "/p.o")
run("kos-cc", "-O2", "-fno-lto", "-ffat-lto-objects", "-fno-builtin-printf", "-c", HERE + "/src/probe2_print.c", "-o", W + "/c.o")
open(W + "/p.ld", "w").write(f"""ENTRY(_probe_entry_dec)
SECTIONS {{ . = 0x{BASE:x};
  .blob : {{ *(.text .text.*) *(.rodata .rodata.* .rodata1) *(.data .data.*) *(.bss .bss.* COMMON) . = ALIGN(4); }}
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) *(.debug*) *(.gnu*) }} }}
""")
run("sh-elf-ld", "-m", "shlelf", "-T", W + "/p.ld", "-o", W + "/p.elf", W + "/p.o", W + "/c.o",
    "--defsym=_printf=0x%x" % printf, "--defsym=_CAL_EVENT=0x" + ev)
run("sh-elf-objcopy", "-O", "binary", W + "/p.elf", W + "/p.bin")
size = os.path.getsize(W + "/p.bin")
tend = taddr + tsize
init = int(re.search(r"\] \.init\s+PROGBITS\s+([0-9a-f]+)", sec).group(1), 16)
if not (tend <= BASE and BASE + size <= init):
    raise SystemExit("blob does not fit: text_end=%x blob=%x+%d init=%x" % (tend, BASE, size, init))
blobnm = {l.split()[2]: int(l.split()[0], 16) for l in run("sh-elf-nm", W + "/p.elf").splitlines() if len(l.split()) == 3}
run("sh-elf-objcopy", "--add-section", ".instr=" + W + "/p.bin", "--set-section-flags", ".instr=alloc,load,readonly,code,contents",
    "--change-section-address", ".instr=0x%x" % BASE, inp, W + "/x.elf")
sec2 = run("sh-elf-readelf", "-S", "-W", W + "/x.elf")
toff2 = int(re.search(r"\] \.text\s+PROGBITS\s+[0-9a-f]+ ([0-9a-f]+)", sec2).group(1), 16)
d = bytearray(open(W + "/x.elf", "rb").read())
patched = []
for R, tag, rn, lit in targets:
    ent = blobnm["_probe_entry_dec"] if R == "dec" else blobnm[entries[(R, rn)]]
    o = toff2 + (lit - taddr)
    cur = struct.unpack("<I", d[o:o + 4])[0]
    assert cur == real[rn], (rn, hex(cur))
    d[o:o + 4] = struct.pack("<I", ent)
    patched.append((rn, lit, ent))
for sn, addr_, foff in data_targets:
    m2 = re.search(r"\] " + re.escape(sn) + r"\s+PROGBITS\s+([0-9a-f]+) ([0-9a-f]+)", sec2)
    o = int(m2.group(2), 16) + (addr_ - int(m2.group(1), 16))   # offset in the rewritten file
    cur = struct.unpack("<I", d[o:o + 4])[0]
    assert cur == syms[dec_fn][0], hex(cur)
    d[o:o + 4] = struct.pack("<I", blobnm["_probe_entry_dec"])
    patched.append((dec_fn + " (data word in " + sn + ")", addr_, blobnm["_probe_entry_dec"]))
open(out, "wb").write(d)
print("probe2: %d bytes at 0x%x (limit %d); %d literals patched:" % (size, BASE, init - BASE, len(patched)))
for rn, lit, ent in patched:
    print("  %-34s literal 0x%08x -> 0x%08x" % (rn, lit, ent))
