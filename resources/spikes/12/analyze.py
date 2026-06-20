import re, glob, os

bw = re.compile(r"\[bw\] role=(\w) entsz=(\d+) pkts/s=([\d.]+) appB/s=([\d.]+) "
                r"wireB/s~=([\d.]+) batches/s=([\d.]+) ents/s=([\d.]+) ents/batch=([\d.]+)")

def stats(path, role):
    apps=[]; wires=[]; entss=[]; pkts=[]; entsz=0
    with open(path, errors="ignore") as f:
        for ln in f:
            m = bw.search(ln)
            if not m or m.group(1) != role: continue
            entsz = int(m.group(2))
            pkts.append(float(m.group(3))); apps.append(float(m.group(4)))
            wires.append(float(m.group(5))); entss.append(float(m.group(7)))
    if not apps: return None
    mean = lambda v: sum(v)/len(v)
    return dict(n=len(apps), entsz=entsz, pkts=mean(pkts), app=mean(apps),
                wire=mean(wires), ents=mean(entss), ent_tick=mean(entss)/20.0)

print(f"{'save':<10} {'ents/tick':>9} {'pkts/s':>7} {'appKB/s':>8} {'wireKB/s':>8} {'B/ent/tick':>11}")
for f in sorted(glob.glob("resources/spikes/12/raw/*-host.log")):
    save = os.path.basename(f).replace("-host.log","")
    s = stats(f, "H")
    if not s: continue
    bpe = s["app"]/(s["ent_tick"]*20.0) if s["ent_tick"] else 0
    print(f"{save:<10} {s['ent_tick']:>9.1f} {s['pkts']:>7.1f} {s['app']/1000:>8.2f} "
          f"{s['wire']/1000:>8.2f} {bpe:>11.1f}")

# join side (own-squad only) - should be flat
jf = sorted(glob.glob("resources/spikes/12/raw/*-join.log"))
if jf:
    s = stats(jf[0], "J")
    if s:
        print(f"\njoin (own squad): {s['ent_tick']:.1f} ents/tick, "
              f"{s['app']/1000:.2f} KB/s app, {s['wire']/1000:.2f} KB/s wire (flat across saves)")

# projections from the measured per-entity cost
ENTSZ = 79; HZ = 20
per_ent = ENTSZ * HZ  # app bytes/s per streamed entity
print(f"\nper-entity app cost = {ENTSZ}B x {HZ}Hz = {per_ent} B/s = {per_ent/1000:.2f} KB/s/entity")
for n in (20, 50, 100, 160):
    appkb = n*per_ent/1000
    # +~32B/pkt overhead, ceil(n/18) datagrams/tick
    import math
    pkts = math.ceil(n/18)*HZ
    wirekb = (n*ENTSZ + math.ceil(n/18)*(2+32))*HZ/1000  # +2B header approx
    print(f"  {n:>3} entities -> {appkb:6.1f} KB/s app, ~{wirekb:6.1f} KB/s wire "
          f"({pkts} pkts/s) per peer")
