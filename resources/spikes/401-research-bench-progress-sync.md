# Spike 401 - Research tech-tree progress/unlock sync

- Type: RUN
- Status: DONE
- Save: sync
- Branch commit: <filled at commit>

## Goal

Close the last "Remaining" item of SYNC_GAPS gap 5 (base-building): research
tech-tree unlocks. Protocol 33 ships `getTechLevel()` as census EVIDENCE only -
no writable surface was mapped, so a tech the host researches never crosses to
the join. This spike answers: where does the unlock live, is the divergence
real, and is there an engine write lever we can drive from plugin context.

## Method

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 401 -Save sync -Seconds 55
```

`SpikeScenario::tick401` (both clients): place a research bench (kind 3, new
`probePlaceMachine` branch - the `sync` save has no baked bench in census
range) and ramp it complete; snapshot the `Research` object; enumerate every
`RESEARCH` GameData through the engine's own predicates; deterministically pick
the first not-known researchable record as the SUBJECT; the HOST fires the
engine's own selection lever on it; both clients poll `isKnown(subject)` for
26 s while the host drives `operate()`.

Static prep (offline, on the installed 1.0.65 exe): string-xref of "Research
already known" / "Cannot research" located the research-UI click handler at
`0x2b65d0`; disassembling it recovered the three levers below. Their base is
`GetModuleHandle(NULL)` located at runtime by a unique 24-byte prologue scan of
`.text` (the running image is base-skewed from the on-disk file, so base+RVA is
wrong - see Findings).

## Raw evidence

Host (`401/raw/host.log`):

```
[r401] lever scan isKnown=00007ff6ed67e430 can=00007ff6ed6820d0 start=00007ff6ed683680
[r401] levers located by prologue scan
[r401] research[0] sid='66290-Newwworld.mod.TECH.1' name='Eagle's Cross' known=0 can=1
[r401] research total=384 known=1
SPIKE 401 store baseline rs=1 research-total=384 picked=1 subject='66290-Newwworld.mod.TECH.1' current='(none)'
SPIKE 401 bench n=2 op=1 tech=1 prog=0.0000 power=1 known=0 cur='' t=9032
SPIKE 401 select rc=1 sid='66290-Newwworld.mod.TECH.1'
SPIKE 401 bench n=3 op=1 tech=1 prog=0.0000 power=1 known=1 cur='' t=10032
[r401] diff 0x00c 00000000->00000001   (at the select tick)
[r401] diff 0x020 00000001->00000002
[r401] diff 0x110 00000001->00000003
SPIKE 401 summary have=1 placed=1 samples=26 subject='66290-Newwworld.mod.TECH.1' known=1 current='(none)'
```

Join (`401/raw/join.log`):

```
SPIKE 401 store baseline rs=1 research-total=384 picked=1 subject='66290-Newwworld.mod.TECH.1' current='(none)'
SPIKE 401 bench n=1  op=-1 ... known=0 ...
SPIKE 401 bench n=26 op=-1 ... known=0 ...
SPIKE 401 summary have=1 placed=0 samples=26 subject='66290-Newwworld.mod.TECH.1' known=0 current='(none)'
```

## Findings

1. The unlock store is `PlayerInterface::technology` (a `Research*` at
   `PlayerInterface+0x38`). It is per-client: a `Research` unlocked on the host
   is NOT unlocked on the join - the real gap.
2. The engine's own research predicates/levers work from plugin context:
   `Research::isKnown(GameData*)`, `Research::canResearch(GameData*,bool,bool)`,
   and `Research::startResearch(GameData*)`.
3. `startResearch(subject)` is a working WRITE lever: it flips the host's
   `isKnown(subject)` from 0 to 1 (and mutates counters in the store) in the
   same tick, surviving every subsequent poll.
4. A `RESEARCH` GameData's `stringID` is a cross-client-stable wire key: both
   clients independently enumerated 384 records and picked the identical
   subject sid (`66290-Newwworld.mod.TECH.1`) by shared enumeration order.
5. On the 1.0.65 build the running image is BASE-SKEWED from the on-disk exe:
   raw base+RVA calls resolve to unrelated code. A unique-prologue `.text` scan
   finds each function at its true runtime address; raw calls must be guarded
   this way (an unguarded/mis-based call crashed both clients, run 211124).
6. `getTechLevel()` and `UseableStuff::progressBarLevel` are NOT the unlock
   surface: both stayed flat (tech=1, prog=0.0) on the host across 26 operate()
   samples while `isKnown(subject)` went 0 -> 1. Bench-local progress is not the
   sync target; the global `Research` completed-set is.

## Validation

1. Store identity + per-client divergence: host `SPIKE 401` line n=3 onward
   `known=1` after `select`, vs join `SPIKE 401` all 26 samples `known=0` for
   the SAME subject sid. `researchQueryBySid` reads `gw->player->technology`
   (`Engine.cpp` `r401Research`); it resolved to a real object both sides
   (`rs=1`, `research-total=384`).
2. Predicates work: `[r401] research[i] ... known=.. can=..` printed real
   values for 384 records via `Research::isKnown`/`canResearch`
   (`401/raw/host.log`); `[r401] research total=384 known=1`.
3. Write lever: host `SPIKE 401 select rc=1` at t=10032 immediately followed by
   `known=1` (was `known=0` at n=1/n=2), plus `[r401] diff 0x00c 0->1` /
   `0x020 1->2` / `0x110 1->3` at that exact tick - the store mutated.
4. Cross-client-stable key: host AND join both logged
   `picked=1 subject='66290-Newwworld.mod.TECH.1'` from independent
   `researchPickSubject` scans (`Engine.cpp`), same sid.
5. Base skew: `[r401] lever scan isKnown=00007ff6ed67e430 ...` (prologue-scan
   addresses) succeeded and the calls ran cleanly; the prior base+RVA build
   logged `[r401] levers REFUSED` / crashed (runs 211124, 212346). Each
   24-byte prologue was verified `count==1` in the file `.text` offline.
6. Wrong surface ruled out: `SPIKE 401 bench` host lines show `tech=1
   prog=0.0000` constant across all 26 samples while `known` flipped - the
   `probeResearchBenchRead` fields do not track the unlock.

## Open questions / hypotheses (UNVALIDATED)

- Whether `startResearch` INSTANTLY completes in a normal game or whether it
  merely QUEUES and the spike's placed-bench/instant context completed it. The
  join apply path only needs "make this sid known", which `startResearch`
  achieves here; if in some contexts it only queues, the apply may need a
  direct completed-set insert. Test: on the join, call `startResearch(sid)` for
  a host-completed tech and re-poll `isKnown` (this is the phase-2
  `research_probe` leg).
- Exact completed-set container layout inside `Research` (isKnown walks a map
  reached via `this+0x10`; the inline 0x800 window only exposed counters at
  0x00c/0x020/0x110). Not needed for sync - `isKnown`/`startResearch` are the
  interface.
- `ManagementScreen::currentResearch` (the UI's selected tech) could not be
  read: the header does not compile under VC100 and its KenshiLib RVAs are in
  remapped space. `probeCurrentResearchSid` returns 0; not required for
  unlock sync.

## Implications for co-op

The research gap is now fully mapped and has a working write lever, so it is
wireable as a HOST-AUTHORITATIVE snapshot (the world-simulation precedent, same
shape as `PKT_PROD`):

- Wire key: the newly-known `RESEARCH` GameData `stringID` (cross-client
  stable, finding 4).
- Host publish: diff the known-set (via `isKnown` over the `RESEARCH`
  enumeration), send new sids as `PKT_RESEARCH` rows, change-gated with a
  safety resend; first-sight sends (host state is the baseline).
- Join apply: `startResearch(sid)` (finding 3) - idempotent (already-known is
  a no-op by `isKnown`), keyed by the wire sid.
- Escape hatch `KENSHICOOP_RESEARCH_SYNC` (default ON, forced OFF in
  `research_probe`).

## Recommended follow-ups

- Phase 2 `research_probe`: host researches one tech, join stays not-known with
  the hatch OFF (divergence), then prove `startResearch(sid)` on the join makes
  it known and sticks (apply-lever validation - resolves the first open
  question).
- Phase 3 `PKT_RESEARCH` (bump PROTOCOL_VERSION 36 -> 37): publish/apply in
  Replicator, prologue-scan the levers once at init.
- Keeper primitives promoted from this spike (kept in `Engine.cpp`, not
  reverted, because phase 2/3 consume them directly): `researchQueryBySid`,
  `researchStartBySid`, `researchPickSubject`, `probeResearchEnum`,
  `r401Research`, prologue-scan lever resolver. `probePlaceMachine` kind 3
  (research bench) also stays.
