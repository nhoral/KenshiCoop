import re, sys, statistics

# Match "[HH:MM:SS.mmm] ... [event] SEND id=N" / "... [event] RECV id=N".
ts = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]")
snd = re.compile(r"\[event\] SEND id=(\d+) ev=(\d+)")
rcv = re.compile(r"\[event\] RECV id=(\d+)")

def stamp(line):
    m = ts.search(line)
    if not m: return None
    h, mi, s, ms = map(int, m.groups())
    return ((h * 60 + mi) * 60 + s) * 1000 + ms

def parse(path, pat):
    d = {}
    with open(path, errors="ignore") as f:
        for ln in f:
            m = pat.search(ln)
            if not m: continue
            t = stamp(ln)
            d.setdefault(int(m.group(1)), t)  # first occurrence
    return d

send = parse(sys.argv[1], snd)
recv = parse(sys.argv[2], rcv)

sids, rids = set(send), set(recv)
print(f"SEND={len(sids)} RECV={len(rids)} "
      f"max_send_id={max(sids) if sids else 0} max_recv_id={max(rids) if rids else 0}")
print(f"contiguous send ids 1..max: {sids == set(range(1, max(sids)+1))}")
lost = sorted(sids - rids)
dup = sorted(rids - sids)
print(f"lost (sent, never recv) = {len(lost)}  spurious (recv, never sent) = {len(dup)}")
if lost[:10]: print("  first lost:", lost[:10])

# send-time spread -> rate
sts = sorted(t for t in send.values() if t is not None)
if len(sts) > 1:
    span = (sts[-1] - sts[0]) / 1000.0
    print(f"send span={span:.1f}s  mean rate={len(sts)/span:.1f} events/s")
    # peak per 1s bucket
    from collections import Counter
    buckets = Counter(int((t - sts[0]) / 1000) for t in sts)
    print(f"peak 1s-bucket send rate = {max(buckets.values())} events/s")

# latency (same machine clock): recv_ts - send_ts per id
lat = [recv[i] - send[i] for i in (sids & rids)
       if send[i] is not None and recv[i] is not None]
if lat:
    lat.sort()
    p = lambda q: lat[min(len(lat)-1, int(q*len(lat)))]
    print(f"latency ms over {len(lat)} matched: min={lat[0]} med={statistics.median(lat):.0f} "
          f"p95={p(0.95)} max={lat[-1]} mean={statistics.mean(lat):.1f}")
