#!/usr/bin/env python3
"""Choose and emit SH-4 IDCT island section offsets from a linked probe ELF."""
import argparse
import re
import subprocess
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("elf")
ap.add_argument("--linker-script", required=True)
ap.add_argument("--block", type=lambda x: int(x, 0))
a = ap.parse_args()
PREFIX = "sh-elf-"

def run(*args):
    return subprocess.run([PREFIX + args[0], *args[1:]], check=True,
                          capture_output=True, text=True).stdout

symbols = {}
for row in run("nm", "-S", "-n", a.elf).splitlines():
    p = row.split()
    if len(p) in (3, 4):
        try:
            name = p[-1]
            size = int(p[1], 16) if len(p) == 4 else 0
            symbols[name] = (int(p[0], 16), size, p[2])
        except ValueError:
            pass

def disasm(name):
    lo, size, _ = symbols[name]
    out = run("objdump", "-d", "--no-show-raw-insn",
              "--start-address=%d" % lo, "--stop-address=%d" % (lo + size), a.elf)
    rows = []
    for row in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", row)
        if m:
            rows.append((int(m.group(1), 16), m.group(2)))
    return rows

def body(name):
    addr, size, _ = symbols[name]
    rows = disasm(name)
    returns = [i for i, (_, ins) in enumerate(rows) if ins.startswith("rts")]
    if not returns:
        raise SystemExit("no rts found in " + name)
    code_end = rows[returns[-1] + 1][0] + 2
    code_rows = [(pc, ins) for pc, ins in rows if pc < code_end]
    pool = set()
    for pc, ins in code_rows:
        m = re.search(r"\b(?:mov\.[lw]|mova)\s+([0-9a-f]{8})\s+<", ins)
        if m:
            pool.add(int(m.group(1), 16) & ~31)
    frame = 0
    pushes = 0
    for _, ins in rows[:20]:
        if "@-r15" in ins:
            pushes += 1
        m = re.search(r"add\s+#(-\d+),r15", ins)
        if m:
            frame += int(m.group(1))
    frame = pushes * 4 - frame
    code_lines = sorted({((pc - addr) // 32) for pc, _ in code_rows})
    pool_lines = sorted({(p - addr) // 32 for p in pool})
    return {"addr": addr, "size": size, "frame": frame,
            "code": code_lines, "pool": pool_lines}

def marker_region(start_name, code_end_name, end_name):
    lo = symbols[start_name][0]
    hi = symbols[end_name][0]
    code_end = symbols[code_end_name][0]
    out = run("objdump", "-d", "--no-show-raw-insn", "--start-address=%d" % lo,
              "--stop-address=%d" % code_end, a.elf)
    rows = []
    for row in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", row)
        if m:
            rows.append((int(m.group(1), 16), m.group(2)))
    pool = set()
    for pc, ins in rows:
        m = re.search(r"\b(?:mov\.[lw]|mova)\s+([0-9a-f]{8})\s+<", ins)
        if m:
            pool.add(int(m.group(1), 16) & ~31)
    code = sorted({((pc - lo) // 32) for pc, _ in rows if (pc & ~31) not in pool})
    pools = sorted({(p - lo) // 32 for p in pool})
    return lo, hi, code, pools

required = ["_ff_simple_idct_put_body", "_ff_simple_idct_add_body",
            "_mpeg_idct_private_stack"]
missing = [s for s in required if s not in symbols]
if missing:
    raise SystemExit("missing required symbols: " + ", ".join(missing))
put = body("_ff_simple_idct_put_body")
add = body("_ff_simple_idct_add_body")
stack_addr, stack_size, _ = symbols["_mpeg_idct_private_stack"]
# Keep the high-water SP at the 16 KB boundary used by the selected set model.
top_off = 16384
frame_sets = set()
for info in (put, add):
    frame_sets.update((((top_off - info["frame"]) >> 5) & 511
                       for _ in [0]))
    frame_sets.update((((top_off - info["frame"]) // 32 + i) & 511
                       for i in range((info["frame"] + 31) // 32)))
crop_addr, _, _ = symbols.get("_ff_cropTbl", (0, 0, ""))
if not crop_addr:
    raise SystemExit("missing _ff_cropTbl; cannot plan crop aliases")
crop_sets = {((x >> 5) & 511) for x in range(crop_addr + 1024, crop_addr + 1280, 32)}
if crop_sets != set(range(32, 40)):
    raise SystemExit("crop identity region must be pinned to sets 32-39 (got %s)" % sorted(crop_sets))
if a.block is not None:
    block_sets = {((x >> 5) & 511) for x in range(a.block, a.block + 768, 32)}
else:
    block_sets = set()
tramp_lo, tramp_hi, tramp_code, tramp_pool = marker_region(
    "_mpeg_idct_island_trampoline_start", "_mpeg_idct_island_trampoline_code_end",
    "_mpeg_idct_island_trampoline_end")
tramp_sets = {x & 255 for x in tramp_code}
tramp_pool_sets = {(x & 511) for x in tramp_pool}
for s in tramp_pool_sets:
    if s in frame_sets | crop_sets | block_sets:
        raise SystemExit("trampoline pool collides with stack/crop/block set %d" % s)

def translated(items, offset, mask):
    return {((offset // 32 + i) & mask) for i in items}

choices = []
tramp_bytes = tramp_hi - tramp_lo
tramp_limit = (tramp_bytes + 31) & ~31
put_section_bytes = (put["size"] + 31) & ~31
for po in range(tramp_limit, 8192, 32):
    pcode = translated(put["code"], po, 255)
    ppool = translated(put["pool"], po, 511)
    if pcode & tramp_sets or ppool & (frame_sets | crop_sets | block_sets | tramp_pool_sets):
        continue
    for ao in range(po + put_section_bytes, 8192, 32):
        acode = translated(add["code"], ao, 255)
        apool = translated(add["pool"], ao, 511)
        if acode & (tramp_sets | pcode):
            continue
        if apool & (frame_sets | crop_sets | block_sets | tramp_pool_sets | ppool):
            continue
        choices.append((max(po, ao), po + ao, po, ao, ppool, apool))
if not choices:
    raise SystemExit("no placement satisfies I-cache and operand-cache constraints")
_, _, po, ao, ppool, apool = min(choices)

script = f"""SECTIONS
{{
  . = ALIGN(16384);
  __mpeg_idct_island_base = .;
  .idct_island_trampoline : {{ KEEP(*(.text.idct_trampoline)) }}
  . = __mpeg_idct_island_base + 0x{po:x};
  .idct_island_put : {{ KEEP(*(.text.idct_put_body)) }}
  . = __mpeg_idct_island_base + 0x{ao:x};
  .idct_island_add : {{ KEEP(*(.text.idct_add_body)) }}
  __mpeg_idct_island_end = .;
}}
INSERT AFTER .text;
"""
Path(a.linker_script).write_text(script)
print("island base: 16 KB aligned; trampoline 0x%08x..0x%08x" % (tramp_lo, tramp_hi))
print("selected offsets: put +0x%x, add +0x%x" % (po, ao))
print("selected pool sets: put %s; add %s" % (sorted(ppool), sorted(apool)))
