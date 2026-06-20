import re, sys, math, statistics

pat = re.compile(r"SPIKE 9 unit role=(\w) t=(\d+) hand=(\d+),(\d+) "
                 r"pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) body=(\d+) combat=(\d)")

def parse(path):
    d = {}
    with open(path, errors="ignore") as f:
        for ln in f:
            m = pat.search(ln)
            if not m: continue
            role, t, hi, hs, x, y, z, body, comb = m.groups()
            key = (hi, hs)
            d.setdefault(key, []).append(
                (int(t), float(x), float(y), float(z), int(body), int(comb)))
    return d

host = parse(sys.argv[1])
join = parse(sys.argv[2])

common = sorted(set(host) & set(join))
honly = set(host) - set(join)
jonly = set(join) - set(host)
print(f"host hands={len(host)} join hands={len(join)} common={len(common)} "
      f"host-only={len(honly)} join-only={len(jonly)}")

# elapsed-t windows present
ht = sorted({t for v in host.values() for (t,*_) in v})
jt = sorted({t for v in join.values() for (t,*_) in v})
print(f"host t-range {ht[0]}..{ht[-1]} ({len(ht)} windows)  "
      f"join t-range {jt[0]}..{jt[-1]} ({len(jt)} windows)")

def last(v): return max(v, key=lambda r: r[0])

# Per-hand last-sample comparison (each client's final observed state of that body)
dists = []
body_agree = comb_agree = 0
print("\nper-hand LAST-sample host vs join:")
for k in common:
    h = last(host[k]); j = last(join[k])
    dx, dy, dz = h[1]-j[1], h[2]-j[2], h[3]-j[3]
    d = math.sqrt(dx*dx+dy*dy+dz*dz)
    dists.append(d)
    if h[4]==j[4]: body_agree += 1
    if h[5]==j[5]: comb_agree += 1
    print(f"  hand={k[0]},{k[1]:>10}  Hpos=({h[1]:.0f},{h[3]:.0f}) Jpos=({j[1]:.0f},{j[3]:.0f}) "
          f"dist={d:7.1f}  body H{h[4]}/J{j[4]} combat H{h[5]}/J{j[5]}  (Ht={h[0]} Jt={j[0]})")

if dists:
    print(f"\nLAST-sample divergence over {len(dists)} common bodies: "
          f"min={min(dists):.1f} med={statistics.median(dists):.1f} "
          f"mean={statistics.mean(dists):.1f} max={max(dists):.1f}")
    print(f"body-state agreement {body_agree}/{len(common)}  "
          f"combat agreement {comb_agree}/{len(common)}")

# t-aligned divergence (treat elapsed t as comparable; caveat: ~load-skew offset).
# For each common hand, match nearest-t join sample to each host sample.
def nearest(samples, t):
    return min(samples, key=lambda r: abs(r[0]-t))
aligned = []
for k in common:
    for hs in host[k]:
        js = nearest(join[k], hs[0])
        if abs(js[0]-hs[0]) > 2000: continue
        dx, dy, dz = hs[1]-js[1], hs[2]-js[2], hs[3]-js[3]
        aligned.append(math.sqrt(dx*dx+dy*dy+dz*dz))
if aligned:
    print(f"\nt-aligned (|dt|<=2s) divergence over {len(aligned)} pairs: "
          f"med={statistics.median(aligned):.1f} mean={statistics.mean(aligned):.1f} "
          f"max={max(aligned):.1f}")
