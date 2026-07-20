# Spike 60 - Bounty/crime sync: authority model & channel design

- Type: DESIGN (no runtime behavior; specifies the follow-up channel)
- Status: AUTHORITY SETTLED - **H2 witness-local CONFIRMED** by the 2026-07-20
  live run (two runs: host-owned Flashbox, then join-owned PC). Channel shape is
  now fixed to the H2 reconciliation (host-authoritative row, published to the
  owner); ready to implement.
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

## LIVE RUN RESULT (2026-07-20) - H2 witness-local CONFIRMED

Two live host+join sessions, `KENSHICOOP_BOUNTY_PROBE=1` on both clients.

**Run A (control, host-owned Flashbox).** A HOST-owned PC committed the crime.
Bounty row appeared on the host. Non-discriminating by construction: owner =
witness = host, so it cannot separate H1 from H2 (documented in spike 59).

**Run B (the discriminator, JOIN-owned PC).** The other PC - hand
`1,3332275456`, **confirmed JOIN-owned**: the JOIN emits its `[stats] SEND`
(`[13:28:40.872] [JOIN] [stats] SEND hand=1,3332275456 ...`) while the HOST
never sends stats for it (host only sends `1,2637329152` and `2,3587646464`).
This PC committed the on-screen crime ("Cometiendo un Crimen (20)", 0x20). The
`[bounty]` timelines for that exact hand:

- **HOST** (`E:\SteamLibrary\steamapps\common\Kenshi\KenshiCoop_host.log`) - the
  row EXISTS and persists, from first sight to end of capture:
  - `[13:27:58.894] [HOST] [bounty] STATE host=1 hand=1,3332275456 crime=5 vs=11624-Dialogue (10).mod expiry=20.0 had=1 rows=1/1`
  - `[13:29:40.894] [HOST] [bounty] ROW host=1 hand=1,3332275456 fac=11624-Dialogue (10).mod amount=500 crimes=0x20 claimed=0`
  - crime state field is `crime=5` on every one of the 186 host samples for this hand.
- **JOIN** (`C:\Users\Zero\Kenshi-Join\KenshiCoop_join.log`) - the OWNER stays
  completely clean for the whole >3.5 min the crime is active:
  - `[13:31:56.428] [JOIN] [bounty] STATE host=0 hand=1,3332275456 crime=3 vs=11624-Dialogue (10).mod expiry=20.0 had=0 rows=0/0`
  - `had=0 rows=0/0` on every one of the 185 join samples for this hand; **zero
    `[bounty] ROW` lines were ever emitted on the join for `1,3332275456`.**
  - crime state field is `crime=3` on every join sample (the transient FSM also
    forks: host sees `crime=5`, owner sees `crime=3`).

**Verdict: H2.** The durable bounty row materialises SOLELY on the HOST's driven
copy of a JOIN-owned character and never on the owner. Ownership does not move
the row; the witness/guard simulation - which is host-side in this coop world -
does. This is the same host-only outcome as the host-owned Flashbox run, but now
with the owner on the OTHER side, which is exactly the discrimination Run A could
not provide.

*Red herring ruled out:* the join log also shows a durable row for hand
`1,169067376` (`fac=defaultEmpireFactionSID amount=2910 crimes=0x0 crime=0`).
That is NOT a counter-example to H2: it is a purely join-local NPC - the HOST log
has **no record of that hand at all** and neither client emits `[stats] SEND`
for it, so it is an ambient NPC in the join's local world, not a player-owned PC
and not evidence of any owner-side crime->assign pipeline.

## Recommended channel - H2 reconciliation (confirmed; the H1 owner-authoritative variant is NOT applicable)

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

### Publish - HOST-authoritative, NOT owner-authoritative (settled by the H2 run)

**Direct answer to the design question:** `publishBounties` must be
**host/witness-authoritative**, the OPPOSITE of the faction channel. Factions
are owner-authoritative because the owning client's engine produces the relation
delta; bounty is not - the H2 run proved the owner's engine never produces the
bounty row at all (join stayed `had=0 rows=0/0` while its own PC was wanted). The
HOST is the only engine that runs witness->`assignBountyForCrimes`, so the HOST
must be the source of the row and publish it DOWN to the owning client (and any
client that must display/enforce it). The channel is therefore host->clients,
keyed per-character - reconciliation, not the one-directional protocol-24 mirror.

- New `Replicator::publishBounties(gw, net)`, gated by a `bountySync_` flag,
  called from the same tick site as `publishFactions`, but running **on the
  host**.
- Subjects: every character whose host-side `BountyManager` holds a row - i.e.
  the host's driven copies of remote-owned PCs (this is where join-owned PCs'
  bounties live) PLUS host-owned PCs. The per-character `ownerId`/hand in the
  packet tells the receiver which of its characters the row belongs to; the
  owning client applies it to its own (currently-clean) copy.
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
