# Spike 60 - Bounty/crime sync: authority model & channel design

- Type: DESIGN (no runtime behavior; specifies the follow-up channel)
- Status: DESIGN READY - blocked on the spike-59 live run before implementation
- Depends on: spike 59 (`59-bounty-crime-probe.md`, the read-only observer)
- Precedent: protocol 24 (faction relations, `PKT_FACTION`) - the closest
  working detour+sample+apply channel; this design is that pattern re-keyed
  per-character.

## Where this stands (evidence in hand, 2026-07-20)

1. **Zero bounty/crime coverage in the wire protocol.** `PacketType` in
   `src/netproto/Wire.h` has 41 packet types (`PKT_HELLO`..`PKT_CAM_HINT`) and
   `EventType` has 8 (`EVT_NONE`..`EVT_DROP_BODY`); none touch crime or bounty.
   The only cross-client coupling that even brushes the system is the RELATION
   side effect the `[fac] AFFECT` detour records - it sees relation
   *consequences* of crimes, never the bounty/crime state itself. Bounty is a
   whole feature with no sync path, not a partially-synced one.

2. **The read lever is real and now compiles.** `Character::crimes` is an
   inline `BountyManager` at `Character+0xF0`, confirmed directly against the
   KenshiLib PDB headers vendored in this repo:
   - `third_party/KenshiLib_deps/KenshiLib/Include/kenshi/Character.h:242`
     -> `BountyManager crimes; // 0xF0 Member`
   - `.../kenshi/BountyManager.h` -> `bounties` (0x00,
     `ogre_unordered_map<Faction*, Bounty>`), `me` (0x40), `committingCrime`
     (0x58), `crimeAgainstFaction` (0x60), `crimeAgainst` hand (0x70),
     `crimeExpiry` (0x90), `prisonSentenceToServe` (0xA0),
     `_hadABountyAssignedForCurrentCrime` (0xA4).
   - `.../kenshi/Bounty.h` -> `Bounty{ int amount (0x0); unsigned int crimes
     (0x4, CrimeEnum bitmask); bool bountyHasBeenClaimedOnce (0x8) }` and the
     16-value `CrimeEnum` (CRIME_NONE..CRIME_UNIFORM_THEFT).

   The spike-59 probe (`bountyProbeTick`, `KENSHICOOP_BOUNTY_PROBE=1`) that
   reads all of the above **was dropped from the 2026-07-19 PR batch for fear
   it would not compile. It compiles.** Built clean on this branch with the
   real v100 toolchain (`scripts/build_plugin.cmd` -> `KenshiCoop.dll`, only the
   pre-existing KenshiLib header warnings). It is now landed here and ready to
   run.

3. **The write levers exist** (`BountyManager.h`, all with RVAs in the PDB):
   `setCrime(CrimeEnum, Faction* against, const hand&)`,
   `notifyCrimeWitnessed(Faction*, const hand&, int expiry, CrimeEnum)`,
   `assignBountyForCrimes(Faction* enforcer)`, `unfairAddToBounty(Faction*, int
   amount)`, `clearBounty(Faction* enforcer)`, `getActualBounty(Faction*)`,
   `getTotalBounty()`.

## Why the design is written but NOT yet implemented

The spike-59 probe has **not been run live** yet (a live run needs a two-client
manual session with a player committing a witnessed crime - a human at the
keyboard on both clients, not something the plugin can self-drive). The spike
itself makes the sync decision contingent on that run: *"No sync design is
implied yet ... that decision belongs after the evidence, not before."*

The divergence question is effectively pre-answered - there is no sync path, so
per-client bounty state **must** fork the moment one client commits a crime the
other's engine does not independently simulate. What the live run actually
decides is the **authority split** (see the open question below), and that
split changes which of the two channel shapes below we build. Committing an
engine-WRITE channel (calling `assignBountyForCrimes` / `unfairAddToBounty` /
`clearBounty` / `setCrime` on a peer's driven copy) that has never run in a live
session would be exactly the "forzar algo a medias o mal probado" this work was
told to avoid, and the faction channel it copies needed real sessions to settle
its echo/reciprocal guards. So: probe + design land now; the write channel lands
after the run confirms the authority split.

## The authority question the live run must settle

Bounty state has two halves on each `BountyManager`:

- **Durable bounty rows** - `bounties[faction] = {amount, crimes, claimed}`:
  the accumulated wanted level per faction. This is the state gameplay cares
  about (guards aggro, bounty-hunter payouts, gate access).
- **Transient crime state machine** - `committingCrime`, `crimeAgainstFaction`,
  `crimeAgainst`, `crimeExpiry`, `prisonSentenceToServe`,
  `_hadABountyAssignedForCurrentCrime`: the "currently doing a crime / serving
  time" scratch state that resolves INTO a durable row.

A crime is committed by character A (owned by one client) but WITNESSED by an
NPC guard. In this coop model world NPCs are host-simulated. So the open
question is **which engine runs the witness->assign pipeline**:

- **(H1) Owner-local assignment.** The owning client's engine runs the full
  crime -> witness -> `assignBountyForCrimes` pipeline on its own PC, so the
  bounty row appears on the OWNER's copy first. -> clean **owner-authoritative**
  sync: owner broadcasts its rows, peer applies to the driven copy. One
  direction, protocol-24 shaped.
- **(H2) Witness-local assignment.** Only the host's guard simulation fires
  `notifyCrimeWitnessed` on the host's DRIVEN copy of a join-owned PC, so the
  bounty appears on the HOST first and the join's own PC stays clean. -> a
  **split** needing bidirectional reconciliation (the host must publish
  "bounty my witness assigned to your character" back to the owner), closer to
  the money-pool reconciliation than to protocol 24.

The spike-59 `[bounty] STATE`/`ROW` timeline answers this directly: diff
host.log vs join.log for the SAME PC hand while that PC commits a witnessed
crime, and watch which side's rows move first and whether `committingCrime`
pulses on the owner or the witness. **Read the run before choosing H1 vs H2.**

## Recommended channel (assuming H1; fall back to H2's reconciliation if the run shows the split)

Re-key the protocol-24 pattern from per-player-faction to
**per-(owner-character, faction)**.

### Wire (protocol bump 43 -> 44, new `PKT_BOUNTY`)

```c
struct BountyPacket {
    u8  type;        // = PKT_BOUNTY
    u32 ownerId;     // network player id of the sender (the character's owner)
    u32 seq;         // per-sender monotonic (stale-row guard, as PKT_FACTION)
    u32 hIndex;      // owning character hand (index,serial) - the per-char key
    u32 hSerial;
    char sid[48];    // faction GameData stringID ("" never sent) - cross-client
    int  amount;     // Bounty::amount (cats) for that (char, faction) row
    u32  crimes;     // Bounty::crimes bitmask (CrimeEnum bits)
    u8   claimed;    // Bounty::bountyHasBeenClaimedOnce
    // Optional (decide from the run): u8 committing / f32 sentence if the
    // prison-sentence state must cross so a captured PC serves time on both
    // copies. Leave OUT of v1 unless the run shows it forking in a way that
    // breaks gameplay - transient scratch is local simulation noise otherwise.
};
```

Identity: faction by `facSidOf` sid (the faction probe already proved sids are
cross-client stable); character by hand (index,serial), the key every other
per-character channel uses. Both are already resolvable in `Engine.h`
(`facSidOf`, `resolveCharByHand`, `readObjectHand`).

### Publish (owner side) - mirror `Replicator::publishFactions`

- New `Replicator::publishBounties(gw, net, ownerId)`, gated by a
  `bountySync_` flag, called from the same tick site as `publishFactions`.
- Subjects: the local player squad (`gw->player->playerCharacters`) - the
  owner's own PCs only, exactly the probe's first subject set.
- Sample ~1 Hz, plus an immediate sample when a **detour** flags a real
  mutation this tick (see below). Diff each (char hand, faction sid) row's
  `{amount, crimes, claimed}` against a silently-seeded baseline (both clients
  loaded the same save -> shared baseline; stream only mid-session movement).
  Monotonic `seq`, `RESEND_MS` safety resend - all as protocol 24.

### Detour (immediacy + cause) - mirror the `affectRelations` detour

`EngineWorld.cpp` already detours both `FactionRelations::affectRelations`
overloads to log `[fac] AFFECT` and record a `FactionDelta` the Replicator
drains for same-tick sampling. Do the same on the bounty assignment seam -
`BountyManager::assignBountyForCrimes` and/or `notifyCrimeWitnessed` (RVAs in
`BountyManager.h`) - to (a) get the `[bounty] AFFECT` cause line the way
relations got theirs and (b) push a `BountyDelta` so the changed row crosses
within a tick instead of up to a sample period later. This detour is also the
cleanest place to learn H1-vs-H2 empirically if the probe run is ambiguous:
whichever client's process the detour fires in IS the assigning engine.

### Apply (peer side) - mirror `Replicator::applyFactions`

- New `Replicator::applyBounties(gw, in)`: drain inbound, reject stale by seq,
  resolve the character by hand and the faction by sid.
- **Echo guard**: update the local baseline for that (char, faction) row
  BEFORE writing, so the write's own mutation is not re-detected next sample
  (the exact bug the faction channel's `fr.known = p.relation` line guards).
- **Convergence-first**: if the driven copy's row already equals the target
  (`getActualBounty`), skip (resend/echo). Otherwise write via the engine's own
  levers, never a raw map poke:
  - increase: `unfairAddToBounty(fac, target - current)` (additive, keeps
    derived expiry/GUI consistent) - preferred over reconstructing via
    `setCrime`+`assignBountyForCrimes`.
  - clear/decrease to zero: `clearBounty(fac)`.
  - `crimes` bitmask / `claimed` flag: fold in only if the run shows they
    matter to gameplay on the peer; `unfairAddToBounty` already moves the
    amount, which is what guards read.
- SEH-guard every engine call (the whole plugin's write contract) and log
  `[bounty] RECV char=.. fac=.. amount=.. was=.. ok=.. seq=..` symmetric to
  `[fac] RECV`.

### Testability

Extract the pure decision (given last-known row + incoming row -> {skip |
send} and {skip | write delta}) into a header-testable free function the way
other channels keep their diff logic side-effect-free, so `build_prototest.cmd`
can cover the seq/echo/convergence logic without a live engine. The engine
read/write shims stay in `EngineCharState.cpp` behind SEH.

## Verification plan (the part that needs a live game)

1. Run spike 59 first: `set KENSHICOOP_BOUNTY_PROBE=1` on BOTH clients, commit a
   witnessed crime with one PC, diff `[bounty]` timelines -> settle H1 vs H2 and
   the minimal carry-set (which fields actually move).
2. Implement the channel per the settled authority split.
3. Live host+join session: one player commits a crime / gets a bounty in front
   of a guard; verify the OTHER client's copy of that PC shows the same bounty
   (`[bounty] RECV ok=1`, and in-game guards react to the driven copy). Pay it
   off / serve the sentence; verify it clears on both.
4. `build_prototest.cmd` green on the extracted decision logic.

Nothing here is committed as engine-write code until step 3 passes.
