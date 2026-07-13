import re, sys, math, statistics

pat = re.compile(r"SPIKE 13 unit role=(\w) t=(\d+) hand=(\d+),(\d+) "
                 r"pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) body=(\d+)")
BODY_DEAD = 4; BODY_RAGDOLL = 2; BODY_DOWN = 1

def parse(path):
    d = {}
    with open(path, errors="ignore") as f:
        for ln in f:
            m = pat.search(ln)
            if not m: continue
            role, t, hi, hs, x, y, z, body = m.groups()
            d.setdefault((hi, hs), []).append(
                (int(t), float(x), float(y), float(z), int(body)))
    return d

host = parse(sys.argv[1]); join = parse(sys.argv[2])
last = lambda v: max(v, key=lambda r: r[0])
common = sorted(set(host) & set(join))
print(f"host hands={len(host)} join hands={len(join)} common={len(common)}")

# How many bodies the HOST reports dead at its last sample, and what the JOIN's
# last sample for the SAME hand shows (does BODY_DEAD propagate as a pose?).
hdead = [k for k in common if last(host[k])[4] & BODY_DEAD]
print(f"\nhost-dead common bodies (last sample): {len(hdead)}")
jdead = jdown = jragdoll = jalive = 0
dists = []
for k in hdead:
    h = last(host[k]); j = last(join[k])
    if j[4] & BODY_DEAD: jdead += 1
    elif j[4] & BODY_RAGDOLL: jragdoll += 1
    elif j[4] & BODY_DOWN: jdown += 1
    else: jalive += 1
    d = math.dist((h[1],h[2],h[3]), (j[1],j[2],j[3]))
    dists.append(d)
print(f"  join shows for those same hands: DEAD={jdead} RAGDOLL={jragdoll} "
      f"DOWN={jdown} ALIVE/other={jalive}")
if dists:
    print(f"  corpse pos divergence (host-dead, last sample): "
          f"min={min(dists):.1f} med={statistics.median(dists):.1f} "
          f"mean={statistics.mean(dists):.1f} max={max(dists):.1f}  (n={len(dists)})")

# Did corpses STOP moving on each client after the kill (t>=10000)? Report the
# max intra-client displacement of each dead body across post-kill samples.
def settle(d, keys):
    moved = []
    for k in keys:
        post = [r for r in d[k] if r[0] >= 12000]
        if len(post) < 2: continue
        xs = [(r[1],r[2],r[3]) for r in post]
        mx = max(math.dist(xs[0], p) for p in xs)
        moved.append(mx)
    return moved
hm = settle(host, hdead); jm = settle(join, hdead)
if hm: print(f"\nhost corpse drift after t=12s: med={statistics.median(hm):.1f} "
             f"max={max(hm):.1f}")
if jm: print(f"join corpse drift after t=12s: med={statistics.median(jm):.1f} "
             f"max={max(jm):.1f}")
