# Spike 59 - Bounty/crime probe (first direct read of the bounty system)

- Type: STATIC (headers) + code (RUN recipe)
- Status: READY (code landed, not yet run)
- Save: any with a wanted PC (a fresh crime works); clean saves stay quiet
- Branch commit: <filled at commit>

## Goal

Give the bounty/crime "remaining edge" of gap 3 its first DIRECT observation
channel. `SYNC_GAPS.md` currently records it as accepted with only indirect
evidence:

> bounties / crime state are a separate engine system (the `[fac] AFFECT`
> detour keeps accumulating cause evidence)

The `[fac] AFFECT` detour sees relation *consequences* of crimes, never the
bounty state itself - there is no read lever anywhere in the plugin for what a
client believes a character's bounty/crime state IS. Before any sync design
(or even a decision that none is needed), we need the same thing every other
gap got first: a per-body, per-client, read-only census of the real engine
state, so a two-client run can show whether and how the two sides fork.

## Method

Read-only diagnostic, gated OFF by default - the jail/shackle probe contract
(observe, log, measure, change nothing):

- `KENSHICOOP_BOUNTY_PROBE=1` -> `[bounty]` observer in
  [EngineCharState.cpp](../../src/plugin/game/EngineCharState.cpp)
  (`bountyProbeTick`, called from the Plugin.cpp tick right after
  `shackleDbgTick`). ~1 Hz over the local player squad + nearby world NPCs
  (`listNpcs`; the sets are disjoint).

### The read lever (why this is now possible)

`Character::crimes` is an **inline `BountyManager` at `Character+0xF0`**
(KenshiLib PDB layout - `kenshi/Character.h`: `BountyManager crimes; // 0xF0`;
the offset was independently re-verified against an external RE offset map of
the same PDB, not derived from any scanning in this repo). The struct we can
read directly (`kenshi/BountyManager.h`):

- `bounties` (+0x00) - `ogre_unordered_map<Faction*, Bounty>`: the actual
  per-faction bounty table (`Bounty` = amount in cats + CrimeEnum bitmask +
  claimed-once flag).
- `me` (+0x40) - back-pointer to the owning Character. **Used as the probe's
  validity sentinel**: `me != c` means the layout does not hold for this build,
  one `SENTINEL-MISMATCH` tripwire line is logged and nothing is parsed.
- `committingCrime` (+0x58), `crimeAgainstFaction` (+0x60), `crimeExpiry`
  (+0x90), `prisonSentenceToServe` (+0xA0),
  `_hadABountyAssignedForCurrentCrime` (+0xA4) - the live in-progress-crime
  state machine.

Field reads only - no engine function is called (the header also names the
levers a future sync could use: `getActualBounty`, `assignBountyForCrimes`,
`clearBounty`, `setCrime`, `notifyCrimeWitnessed`). The inline-member size is
already load-bearing in this codebase: `isChained` (0x320) and `slaveOwner`
(0x328) sit AFTER `crimes` and read correctly everywhere, so the 0xF0 layout
is transitively exercised by every existing furniture/shackle read.

### Log surface

- `[bounty] STATE host= hand= crime= vs= expiry= sentence= had= rows=` - one
  line per body with live state (committing a crime OR at least one bounty
  row); quiet bodies are skipped so a clean save logs nothing.
- `[bounty] ROW host= hand= fac=<sid> amount= crimes=0x.. claimed=` - one line
  per per-faction bounty entry. Factions are keyed by GameData stringID
  (`facSidOf`), which the faction probe already showed is cross-client stable -
  so ROW lines from host.log and join.log diff directly.
- `[bounty] SCAN host= n= valid= active=` - 10 s heartbeat; `valid == n` is
  the layout-health signal even on a fully quiet run.
- `[bounty] SENTINEL-MISMATCH ...` - once per session, the tripwire.

All per-body reads are SEH-guarded; the bounties-map walk lives in an
unguarded shim under the caller's SEH frame (the C2712 `factionBySidC`
pattern).

## Reproduce

Manual two-client session (partitioned or not), both sides with the env set:

```
set KENSHICOOP_BOUNTY_PROBE=1
```

then commit a witnessed crime with one client's PC (steal in a shop, assault
a guard), get caught / pay it off / serve a sentence, and diff the `[bounty]`
timelines of host.log vs join.log per hand + faction sid.

## What the run should answer

1. **Do bounty rows fork cross-client?** (Expected: yes - everything about the
   system is local; the only cross-client coupling seen so far is the relation
   side effect via `[fac] AFFECT`.) The STATE/ROW diff per (hand, faction sid)
   is the direct evidence either way.
2. **Which fields move, and when**: does `committingCrime` pulse and clear, is
   `crimeExpiry` wall-clock or game-clock, does `_hadABountyAssignedForCurrentCrime`
   gate re-assignment - the change timeline tells us which of the fields a
   minimal sync would even need to carry.
3. **Whose bounty state matters**: PC-only, or do captured world NPCs carry
   rows the peer's copy lacks (bounty-hunting gameplay: the join sells a
   target the host's engine thinks is clean)?
4. **Sentinel health across builds** (GOG vs Steam parity ran clean for every
   other Character offset; `valid == n` in SCAN confirms it for +0xF0).

## Non-goals

No sync design is implied yet. If the run shows rows forking (expected), the
candidate scope - owner-authoritative `Bounty` rows keyed by (owner hand,
faction sid), applied via `setCrime`/`clearBounty` or direct row writes - is a
protocol-24-shaped follow-up, but that decision belongs after the evidence,
not before. The probe itself ships knob-gated OFF and changes no behavior.
