import re, sys
from collections import Counter

# Ground truth: who was ever observed attacking whom (attacker_idx,ser -> victim_idx,ser)
ct = re.compile(r"SPIKE 11 ct role=\w t=\d+ hand=(\d+),(\d+) tgt=(\d+),(\d+) inc=(\d)")
# Events: victim = hand idx,ser (fields 4,5); actor = actor idx,ser
snd = re.compile(r"\[event\] SEND id=(\d+) ev=(\d+) "
                 r"hand=\d+,\d+,\d+,(\d+),(\d+) actor=(\d+),(\d+)")

host = sys.argv[1]
observed = set()        # (attacker, victim) pairs seen with a live target
targeters = {}          # victim -> set of attackers ever seen targeting it
per_tick = {}           # host tick t -> set of attacker->victim pairs (inc=1)
tline = re.compile(r"SPIKE 11 ct role=H t=(\d+)")
with open(host, errors="ignore") as f:
    for ln in f:
        m = ct.search(ln)
        if not m: continue
        hi, hs, ti, ts, inc = m.groups()
        if (ti, ts) == ("0", "0"): continue
        atk = (hi, hs); vic = (ti, ts)
        observed.add((atk, vic))
        targeters.setdefault(vic, set()).add(atk)
        tm = tline.search(ln)
        if tm and inc == "1":
            per_tick.setdefault(tm.group(1), set()).add((atk, vic))

peak = max((len(v) for v in per_tick.values()), default=0)
print(f"peak concurrent attacker->victim pairs at one host tick: {peak}")

ev_name = {"1": "KO", "2": "DEATH", "3": "REVIVE"}
tot = Counter(); attributed = Counter(); correct = Counter(); wrong = []
with open(host, errors="ignore") as f:
    for ln in f:
        m = snd.search(ln)
        if not m: continue
        eid, ev, vi, vs, ai, as_ = m.groups()
        tot[ev] += 1
        victim = (vi, vs); actor = (ai, as_)
        if actor == ("0", "0"):
            continue  # unattributed (non-combat cause / no recent attacker)
        attributed[ev] += 1
        if (actor, victim) in observed:
            correct[ev] += 1
        else:
            wrong.append((eid, ev_name.get(ev, ev), actor, victim,
                          sorted(targeters.get(victim, set()))))

print(f"observed attacker->victim pairs: {len(observed)}   victims targeted: {len(targeters)}")
for ev in sorted(tot):
    print(f"  ev={ev}({ev_name.get(ev,'?')}): total={tot[ev]} "
          f"attributed={attributed[ev]} actor-seen-targeting-victim={correct[ev]} "
          f"unattributed(actor=0)={tot[ev]-attributed[ev]}")
print(f"\nMIS-ATTRIBUTIONS (stamped actor NEVER seen targeting victim): {len(wrong)}")
for eid, ev, a, v, seen in wrong[:20]:
    print(f"  id={eid} {ev} actor={a[0]},{a[1]} victim={v[0]},{v[1]} "
          f"actual_targeters={['%s,%s'%t for t in seen] or 'NONE'}")
