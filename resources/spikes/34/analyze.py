import re, sys
from datetime import datetime

# Align host vs join SPIKE 34 samples by WALL CLOCK (each client's elapsed t is its
# own; the only shared axis is the [HH:MM:SS.mmm] log timestamp) and report where the
# two clients' (paused, frameSpeedMult) state DISAGREES - that is the test of whether
# pause / game-speed is synchronized across the co-op session.
line = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SPIKE 34 time role=(\w) "
                  r"t=\d+ paused=(\d) fsm=([\d.]+)")

def parse(path):
    out = []
    with open(path, errors="ignore") as f:
        for ln in f:
            m = line.search(ln)
            if not m: continue
            hh, mm, ss, ms, role, p, fsm = m.groups()
            t = int(hh)*3600 + int(mm)*60 + int(ss) + int(ms)/1000.0
            out.append((t, int(p), float(fsm)))
    return out

H = parse(sys.argv[1]); J = parse(sys.argv[2])
print(f"host samples={len(H)} join samples={len(J)}")

def nearest(arr, t):
    return min(arr, key=lambda r: abs(r[0]-t))

# effective-running = not paused AND fsm>0
def state(p, fsm): return "PAUSED" if (p or fsm == 0.0) else (f"RUN x{fsm:g}")

disagree = 0; compared = 0
print("\nwall-aligned host vs join (only |dt|<=1s):")
for (t, p, fsm) in H:
    jt, jp, jfsm = nearest(J, t)
    if abs(jt - t) > 1.0: continue
    compared += 1
    hs, js = state(p, fsm), state(jp, jfsm)
    if hs != js:
        disagree += 1
        print(f"  wall={t:.1f}  HOST={hs:<8} JOIN={js:<8}  <-- DIVERGE")
print(f"\ncompared {compared} wall-aligned pairs; "
      f"host/join effective-state DISAGREE in {disagree}")

# probe-lever effect on the host: distinct (paused,fsm) states the host passed through
seen = []
for (t, p, fsm) in H:
    s = (p, fsm)
    if not seen or seen[-1] != s: seen.append(s)
print("\nhost state transitions (paused,fsm):", seen)
