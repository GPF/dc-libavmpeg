#!/usr/bin/env python3
"""Measured layout gate: is this build in the fast operand-cache band?

usage: layout_guard.py PROBE_LOG [--max-idct 6000]

Reads the last "@E=04 ..." block printed by a tools/probe2.py build (event 04 =
operand-cache read misses) and checks the IDCT-region read misses per frame.
Fast layouts measure about 3.6-4.3 K per frame there (whole decode about 27 K); the
+12% layouts measure about 27.9 K (whole decode about 51 K). A timing result only counts
when this passes. Exit status 0 = pass, 1 = fail, 2 = no data.
"""
import re, sys
log = open(sys.argv[1]).read()
mx = int(sys.argv[sys.argv.index("--max-idct") + 1]) if "--max-idct" in sys.argv else 6000
blocks = re.findall(r"@E=(\w+) (\w+) t(\d) sum=([0-9a-f]+) n=(\d+)", log)
if not blocks or blocks[-1][0] != "04": print("no event-04 probe data"); sys.exit(2)
last = {}
for ev, name, i, s, n in blocks[-7:]:
    last[(name, int(i))] = (int(s, 16), int(n))
calls = sum(last[("dec", i)][1] for i in range(4))
frames = max(sum(last[("dec", i)][1] for i in (1, 2, 3)), 1)   # pictures returned (calls != frames when input is fed in chunks)
idct = last[("idct", 0)][0] / frames
whole = sum(last[("dec", i)][0] for i in range(4)) / frames
print("decode calls %d; IDCT-region read misses/frame %.0f (limit %d); whole-decode %.0f/frame" % (calls, idct, mx, whole))
ok = idct <= mx
print("PASS: fast band" if ok else "FAIL: IDCT operand-cache aliasing (slow layout)")
sys.exit(0 if ok else 1)
