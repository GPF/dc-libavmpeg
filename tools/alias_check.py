#!/usr/bin/env python3
"""Check IDCT island I-cache placement and operand-cache aliases."""
import argparse
import re
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument("elf")
ap.add_argument("--sp-wrapper", "--sp-idct", dest="sp_wrapper", required=True,
                help="r15 at entry to the assembly trampoline")
ap.add_argument("--block", help="runtime address of the 768-byte MPEG block buffer")
ap.add_argument("--stack-shift", type=int, default=0)
a = ap.parse_args()

def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout

symbols = {}
for row in run("sh-elf-nm", "-S", "-n", a.elf).splitlines():
    p = row.split()
    if len(p) in (3, 4):
        try:
            symbols[p[-1]] = (int(p[0], 16), int(p[1], 16) if len(p) == 4 else 0, p[2])
        except ValueError:
            pass

def need(name):
    if name not in symbols:
        raise SystemExit("missing ELF symbol: " + name)
    return symbols[name]

sections = {}
for row in run("sh-elf-objdump", "-h", a.elf).splitlines():
    m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)", row)
    if m:
        sections[m.group(1)] = (int(m.group(3), 16), int(m.group(2), 16))

island_sections = (".idct_island_trampoline", ".idct_island_put", ".idct_island_add")
for name in island_sections:
    if name not in sections:
        raise SystemExit("missing pinned island output section: " + name)
base, _ = sections[island_sections[0]]
if base & 0x3fff:
    raise SystemExit("island base is not 16 KB aligned: 0x%08x" % base)

def disasm(addr, size):
    out = run("sh-elf-objdump", "-d", "--no-show-raw-insn",
              "--start-address=%d" % addr, "--stop-address=%d" % (addr + size), a.elf)
    return [(int(m.group(1), 16), m.group(2)) for row in out.splitlines()
            for m in [re.match(r"\s*([0-9a-f]+):\s+(.*)", row)] if m]

def pool_slots(rows):
    slots = set()
    for _, ins in rows:
        m = re.search(r"\b(?:mov\.[lw]|mova)\s+([0-9a-f]{8})\s+<", ins)
        if m:
            slots.add(int(m.group(1), 16))
    return slots

def body_code_rows(rows):
    returns = [i for i, (_, ins) in enumerate(rows) if ins.startswith("rts")]
    if not returns:
        raise SystemExit("no rts found in IDCT body")
    code_end = rows[returns[-1] + 1][0] + 2
    return [(pc, ins) for pc, ins in rows if pc < code_end]

def sets_for_range(addr, size, mask):
    return {((x >> 5) & mask) for x in range(addr & ~31, addr + size, 32)}

def sets_for_rows(rows, mask):
    return {((pc >> 5) & mask) for pc, _ in rows}

def check_pool_section(name, slots):
    lo, size = sections[name]
    bad = [p for p in slots if not (lo <= p < lo + size)]
    if bad:
        raise SystemExit("%s literal pool outside its pinned output section: %s" %
                         (name, ["0x%08x" % x for x in bad]))

put_addr, put_size, _ = need("_ff_simple_idct_put_body")
add_addr, add_size, _ = need("_ff_simple_idct_add_body")
tramp_start = need("_mpeg_idct_island_trampoline_start")[0]
tramp_end = need("_mpeg_idct_island_trampoline_end")[0]
tramp_code_end = need("_mpeg_idct_island_trampoline_code_end")[0]
tramp_rows = disasm(tramp_start, tramp_code_end - tramp_start)
put_rows = disasm(put_addr, put_size)
add_rows = disasm(add_addr, add_size)
put_code = body_code_rows(put_rows)
add_code = body_code_rows(add_rows)
tramp_pool = pool_slots(tramp_rows)
put_pool = pool_slots(put_code)
add_pool = pool_slots(add_code)
check_pool_section(island_sections[0], tramp_pool)
check_pool_section(island_sections[1], put_pool)
check_pool_section(island_sections[2], add_pool)

icache = {
    "trampoline": sets_for_rows(tramp_rows, 255),
    "put_body": sets_for_rows(put_code, 255),
    "add_body": sets_for_rows(add_code, 255),
}

stack_addr, stack_size, _ = need("_mpeg_idct_private_stack")
private_top = stack_addr + stack_size - a.stack_shift
if stack_addr < 0x8cff0000 or private_top > 0x8d000000:
    raise SystemExit("private stack must remain in KOS's 16 MB main-thread stack range")
if private_top != 0x8cff4000 - a.stack_shift:
    raise SystemExit("private stack top moved from the planned cache-set address")
crop_addr = need("_ff_cropTbl")[0]

def frame_size(rows):
    pushes = 0
    adjustment = 0
    for _, ins in rows[:20]:
        if "@-r15" in ins:
            pushes += 1
        m = re.search(r"add\s+#(-\d+),r15", ins)
        if m:
            adjustment += int(m.group(1))
    return pushes * 4 - adjustment

def frame_sets(size):
    return sets_for_range(private_top - size, size, 511)

sets = {
    "F_put": frame_sets(frame_size(put_rows)),
    "F_add": frame_sets(frame_size(add_rows)),
    "L_trampoline": {((x >> 5) & 511) for x in tramp_pool},
    "L_put": {((x >> 5) & 511) for x in put_pool},
    "L_add": {((x >> 5) & 511) for x in add_pool},
    "C_identity": sets_for_range(crop_addr + 1024, 256, 511),
}
if sets["C_identity"] != set(range(32, 40)):
    raise SystemExit("ff_cropTbl identity region is not pinned to sets 32-39: %s" %
                     sorted(sets["C_identity"]))
sp = int(a.sp_wrapper, 0) - a.stack_shift
sets["T_saves"] = {((x >> 5) & 511) for x in (sp - 4, sp - 8)}
if a.block:
    block = int(a.block, 0)
    sets["B_blocks"] = sets_for_range(block, 768, 511)

print("island output sections:")
for name in island_sections:
    lo, size = sections[name]
    print("  %-24s 0x%08x..0x%08x" % (name, lo, lo + size))
print("I-cache set ranges (256 sets):")
for name, values in icache.items():
    print("  %-12s %s" % (name, sorted(values)))
for x, y in (("trampoline", "put_body"), ("trampoline", "add_body"), ("put_body", "add_body")):
    overlap = sorted(icache[x] & icache[y])
    if overlap:
        raise SystemExit("I-cache set overlap %s x %s: %s" % (x, y, overlap))
print("operand-cache sets (512 sets; wrapper SP 0x%08x):" % sp)
for name, values in sets.items():
    print("  %-16s %s" % (name, sorted(values)))
failed = []
advisory = []
names = list(sets)
for i, x in enumerate(names):
    for y in names[i + 1:]:
        overlap = sorted(sets[x] & sets[y])
        if overlap:
            if x.startswith("F_") and y.startswith("F_"):
                advisory.append((x, y, overlap, "same private stack reused sequentially"))
            elif "B_blocks" in (x, y):
                advisory.append((x, y, overlap, "block-buffer placement is not pinned"))
            else:
                failed.append((x, y, overlap))
for x, y, overlap in failed:
    print("  ALIAS %-16s x %-16s %s" % (x, y, overlap))
for x, y, overlap, why in advisory:
    print("  NOTE  %-16s x %-16s %s (%s)" % (x, y, overlap, why))
if failed:
    raise SystemExit("operand-cache aliases violate island placement constraints")
if advisory:
    print("PASS: required island constraints hold; advisory aliases above remain to assess")
else:
    print("PASS: I-cache sets are disjoint; literal pools are in pinned sections; no modeled operand aliases")
