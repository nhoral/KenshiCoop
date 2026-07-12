# KenshiCoop architecture review

Date: 2026-07-11  
Status: Recommended direction  
Scope: authoritative host, late join/reconnect, engine access, and replication
structure

## Executive decision

Do **not** abandon the authoritative-host model and do **not** attempt lockstep.
The difficult behavior is primarily a mismatch between replication semantics,
not proof that host authority is the wrong architecture.

Adopt a hybrid model:

1. **Checkpoint** — a host save remains the authoritative world baseline for a
   new session participant.
2. **Reliable journal** — durable changes after the checkpoint are sequenced,
   retained, and replayed.
3. **Native object snapshots** — use Kenshi's serializers for cold-path
   character/building/item bootstrap and repair after live-apply safety is
   proven.
4. **Semantic transactions** — retain explicit protocols for conserved actions
   such as pickup, inventory transfer, construction, and production.
5. **Ephemeral streams** — retain compact newest-wins motion and other
   high-frequency state.

The current implementation should be decomposed around those five semantics
before more packet types are added. This is a refactor of responsibilities, not
a rewrite of the working transport, proxy, intent, interpolation, or save
transfer systems.

## What "seamless join" can realistically mean

Kenshi's world is not designed for arbitrary live replacement underneath an
already running simulation. The safest attainable near-term experience is:

- no manual save copying or pre-seeding;
- the host continues playing;
- the joining client automatically receives and loads a host checkpoint;
- changes made while that client loads are buffered and replayed;
- the client cuts over to live replication without requiring both players to
  restart.

That can be seamless as a product flow even if the joining client crosses one
normal Kenshi load boundary.

A truly load-screen-free join requires constructing or mutating every relevant
live engine object in dependency order. Native serializers make that more
plausible, but spike 402 proves capture only—not safe live apply. It should be a
research goal, not the critical path.

## Findings

### 1. The architecture is overloaded, not directionless

The implementation has accumulated many valid features in two broad modules:

- `Engine.cpp` is both an engine access layer and a large collection of domain
  operations.
- `Replicator.cpp` owns continuous state, snapshots, events, transactions,
  caches, late-join replay, and parts of session recovery.

`Plugin.cpp` then orchestrates networking, scenario behavior, save transfer,
load gates, and replication ticks. The result is high coupling between session
lifecycle and feature state.

This makes every new sync feature appear to require:

- another packet;
- another cache;
- another send timer;
- another reset path;
- another late-join replay branch;
- another conditional in the main tick.

The problem is not simply file length. The problem is that different
correctness models are hidden behind one generic idea of "replication."

### 2. Five different correctness models are currently mixed together

**Ephemeral state**

Examples: transform, facing, speed, animation hints.

Correctness rule: newest value wins. Packet loss is acceptable; old state
should not block new state.

**Durable snapshots**

Examples: money, faction relation, research, medical/body state, completed
building state.

Correctness rule: idempotent replacement at a known revision. Late joiners need
the latest value, not every prior update.

**Events**

Examples: attack intent, alarm/notification, doors, explicit actions.

Correctness rule: ordered, reliable, deduplicated delivery. Replaying an event
may be wrong even when replaying a snapshot is safe.

**Transactions**

Examples: pickup, drop, inventory transfer, construction material use,
production output.

Correctness rule: conserve objects/resources exactly once. These need operation
IDs, validation, commit/reject behavior, and reconciliation.

**Checkpoints/session transitions**

Examples: save transfer, load coordination, reconnect epoch, late-join cutover.

Correctness rule: atomic session generation and sequence boundaries. No pointer
or packet from the old generation may leak into the new one.

Treating these as one replication subsystem is the main source of complexity.

### 3. The engine surface is larger than the current wrappers imply

The project can call more native engine functions than previously assumed.
There are three practical sources:

1. KenshiLib declarations already carrying RVAs;
2. virtual-table methods and `_NV_` twins exposed by KenshiLib;
3. functions discovered from the exact runtime image by signature, call-site,
   RTTI/vtable, or disassembly analysis.

Spike 402 successfully called `Character::serialise` and
`GameDataContainer::save/load` through these mappings. A character was captured
into an isolated container and round-tripped as an approximately 6.7 KiB
payload with six state records, one object instance, and nine container records.

This is the strongest newly unlocked primitive. One native snapshot includes a
cross-section of state that currently spans several packet families.

### 4. The previous RVA problem was partly an image-identity problem

The running module is:

`...\Kenshi\RE_Kenshi\kenshi_x64.exe`

The root-level `kenshi_x64.exe` is a different PE image. Their sizes, PE
timestamps, image sizes, and SHA-256 hashes differ. RVAs derived from the root
image cannot be assumed to describe the `RE_Kenshi` runtime image.

The research functions from spike 401 resolve to stable RVAs in the actual
runtime image on both host and join processes. Therefore:

- ASLR is not an obstacle; use `module base + validated RVA`;
- a known RVA is safe only for an exact build fingerprint;
- a signature scan is a discovery/version-fallback mechanism;
- every resolved entry point should still have byte/prologue and section
  validation before use.

This should become one centralized engine-capability system rather than custom
address logic in individual features.

### 5. Native serialization helps, but it does not erase domain semantics

Native snapshots are well suited to:

- late-join bootstrap after a checkpoint;
- reconnect repair;
- drift detection and targeted resynchronization;
- compact diagnostic capture;
- reducing hand-maintained field lists.

They are not yet suitable for:

- 20 Hz transforms;
- transactional inventory or world-item movement;
- applying repeatedly to a live character without idempotency evidence;
- resolving arbitrary references outside the captured object graph;
- replacing save/load as the first whole-world baseline.

Using the serializer everywhere would move complexity into opaque engine side
effects. The hybrid model uses it where its broad coverage is an advantage and
keeps explicit protocols where operation semantics matter.

### 6. Zone mapping is useful but not a blocker

The zone/cell APIs can improve interest management, snapshot partitioning, and
resync scope. They do not solve identity, authority, conservation, or atomic
join cutover.

Current radius/visibility selection is sufficient while correctness work
continues. Zone-based subscriptions should follow the checkpoint/journal
boundary, not precede it.

### 7. Current test results are useful but overstate coverage

Recent travel-parity evidence supports the transform/proxy path. Several
scenario outcomes are still observational or can be reported as passing after a
skip. That is insufficient for refactoring session and late-join behavior.

Before changing the join path:

- skipped scenarios must be represented as `SKIP`, never `PASS`;
- save/load and late-join scenarios need machine-checkable phase assertions;
- failure output must identify checkpoint, replay, or live-cutover phase;
- host and join logs need the same session epoch and journal sequence markers.

## Target architecture

### Session plane

`SessionCoordinator` owns:

- peer handshake and compatibility;
- authority/session epoch;
- checkpoint request, transfer, load, and acknowledgement;
- journal high-water marks and replay;
- reconnect and full-resync decisions;
- the one atomic transition into live state.

It does not know how to mutate a character, building, or inventory.

### Engine plane

`EngineGateway` owns:

- exact runtime-image fingerprint;
- address/capability registry;
- known RVA and signature resolution;
- main-thread assertions and guarded native calls;
- object lookup/factory adapters;
- native snapshot capture/load/apply adapters;
- event-hook installation and removal.

Feature modules do not use raw addresses or cache unmanaged engine pointers
across session/load generations.

### Identity plane

`IdentityRegistry` owns:

- host authoritative `hand`;
- proxy and local character association;
- content IDs versus runtime instance IDs;
- building/item stable identity;
- deferred references;
- per-session generation and invalidation.

No feature should invent a second fallback identity scheme.

### Replication plane

Split by correctness model, not by packet number:

- `MotionChannel` — unreliable sequenced transforms and interpolation input.
- `SnapshotChannel<T>` — revisioned, idempotent latest-state replacement.
- `EventChannel<T>` — reliable ordered deduplicated event delivery.
- `TransactionChannel<T>` — request, authority validation, commit/reject, and
  reconciliation.
- `ExistenceChannel` — authoritative spawn/despawn/census and proxy lifecycle.
- `NativeSnapshotChannel` — bulk cold-path capture and repair.

Domain adapters such as `CharacterSync`, `InventorySync`, `BuildingSync`, and
`ResearchSync` select one of those delivery semantics. They should not own the
session transport or late-join lifecycle.

### Transport plane

Keep ENet/Steam tunneling, but give traffic classes independent backpressure:

- unreliable state stream;
- reliable control/events/transactions;
- reliable bulk checkpoint/native snapshot transfer.

Bulk save chunks should not head-of-line block control acknowledgements or
transaction commits. Adding a third ENet channel is a small change but should
be validated through both direct and Steam transports.

## Authoritative join protocol

### 1. Negotiate

Host and join exchange:

- protocol version;
- game/runtime-image fingerprint;
- mod/content manifest fingerprint;
- serializer capability version;
- requested session and reconnect token, if any.

An incompatible client is rejected before any world mutation.

### 2. Establish an epoch and checkpoint sequence

The host assigns a new peer to session epoch `E` and records journal sequence
`S0`. It begins retaining all durable events/transactions after `S0`.

### 3. Produce and transfer the baseline

Near-term baseline: a normal host save taken for join. Transfer it on the bulk
channel with:

- transfer ID;
- epoch and `S0`;
- file manifest;
- chunk index/count;
- total length and content hash;
- resumable acknowledgement state.

The host does not load the checkpoint and does not stop live simulation beyond
whatever Kenshi itself requires to save.

### 4. Load only on the joining client

The join writes the checkpoint to an isolated staging directory, verifies the
manifest, loads it, and waits for the existing post-load readiness gates.

Every engine pointer and replication cache is invalidated by generation before
load and reacquired after readiness.

### 5. Reconcile identity and cold state

The host sends:

- authoritative existence census;
- identity map/high-water marks;
- latest native or explicit durable snapshots that supersede checkpoint state.

Initially, explicit snapshots can remain. Native snapshots replace selected
families only after the live-apply ladder passes.

### 6. Replay the durable journal

The host sends reliable records in `(S0, S1]`. The join acknowledges contiguous
sequence progress. Transactions are deduplicated by operation ID; snapshots are
deduplicated by entity revision.

### 7. Cut over atomically

When the join has:

- loaded epoch `E`;
- reconciled existence and identity;
- applied durable sequence `S1`;
- received a current motion keyframe;

the coordinator marks the peer live. Old-epoch messages are discarded.

## Engine address policy

Every manually used native function should have one `AddressSpec` containing:

- symbolic capability name;
- supported build fingerprints;
- known RVA per build;
- expected executable section;
- expected prologue/signature and optional mask;
- calling convention and typed wrapper;
- required game/thread phase;
- failure policy.

Resolution order:

1. identify the actual process module with `GetModuleFileName`;
2. fingerprint the mapped runtime image;
3. look up the exact-build RVA;
4. validate bounds, executable section, and bytes;
5. optionally scan a unique signature when a known build has moved;
6. expose the capability only after validation;
7. disable the dependent feature on unknown/ambiguous results.

Do not:

- derive an RVA from the root launcher and apply it to `RE_Kenshi`;
- call the first pattern match without uniqueness and call-site validation;
- use SEH as address validation;
- invoke native mutators off the game thread;
- keep native object pointers through save/load.

## Refactor plan

### Phase 0 — Freeze and measure

Goal: create a trustworthy baseline before structural work.

Work:

- pause new packet families except critical fixes;
- correct `SKIP` versus `PASS` scenario reporting;
- add session epoch, checkpoint ID, journal sequence, and peer phase to logs;
- record current packet counts/rates and save-transfer timings;
- add deterministic late-join, reconnect, and load-generation scenarios;
- preserve spike 402 as a gated probe and document its evidence.

Exit criteria:

- current direct and Steam paths have reproducible baseline verdicts;
- every session test identifies the exact failed phase;
- no skipped prerequisite can produce a pass.

### Phase 1 — Centralize engine capabilities

Goal: make newly discovered native functions safe and repeatable.

Work:

- introduce `EngineGateway`/`AddressRegistry`;
- fingerprint the actual `RE_Kenshi` process image;
- migrate spike-401 custom scanners into typed capability specs;
- register existing KenshiLib-resolved serializers as capabilities;
- add startup capability logging without absolute-address dependence;
- fail closed on unknown builds.

Exit criteria:

- host and join resolve the same capability set and RVAs;
- deliberately corrupt fingerprints/signatures disable calls cleanly;
- no domain module performs its own raw RVA arithmetic.

### Phase 2 — Decompose without changing the wire

Goal: reduce coupling before changing behavior.

Work:

- extract `SessionCoordinator`;
- extract `IdentityRegistry` and central generation invalidation;
- separate motion, existence, snapshots, events, and transactions behind the
  current packet encoders;
- replace scattered `clear/reset` calls with one session-lifecycle contract;
- make replay/cache ownership explicit for every durable packet.

Exit criteria:

- packet bytes and protocol version are unchanged;
- baseline scenarios remain equivalent;
- a module inventory states the authority, delivery semantic, identity key,
  replay rule, and reset rule for each message.

### Phase 3 — Prove native live apply

Goal: decide exactly where native snapshots may replace manual fields.

Work:

- transfer a detached character payload host-to-join;
- load it into a fresh container on the join;
- apply to a disposable/new object first;
- test double-apply idempotency;
- then test an existing proxy across UI, combat, carried, medical, save/load,
  and zone-transition states;
- audit external references and owned-subobject duplication;
- repeat isolated capture/apply work for buildings and items.

Exit criteria:

- repeated apply does not increase instances, inventory entries, components, or
  stale references;
- all touched state is enumerated;
- failure can roll back or request a full checkpoint;
- per-object payloads have explicit size/frequency budgets.

Decision gate:

- if safe: use native snapshots for cold character state and later selected
  building/item state;
- if unsafe: use them only for detached comparison, diagnostics, and checkpoint
  tooling. The overall hybrid architecture still stands.

### Phase 4 — Add checkpoint plus journal join

Goal: remove manual shared-save prerequisites.

Work:

- implement checkpoint IDs and session epochs;
- retain durable journal entries while a peer joins;
- separate bulk traffic from reliable control traffic;
- verify checkpoint manifests and hashes;
- stage and load the save only on the joining client;
- reconcile, replay, and perform an atomic live cutover;
- bound journal memory and restart from a newer checkpoint when a join takes too
  long.

Exit criteria:

- a client with no matching local save can join automatically;
- host mutations during join appear exactly once after cutover;
- packet loss/disconnect can resume or restart without duplicated transactions;
- old-epoch traffic cannot mutate the newly loaded world.

### Phase 5 — Replace polling with validated event edges

Goal: reduce frame scanning and improve causality.

Work:

- prioritize hooks for spawn/despawn, item drop/pickup, damage/death, inventory
  mutation, building state, and save/load boundaries;
- enqueue lightweight immutable observations in hooks;
- process networking and engine mutation at the established main-thread safe
  point;
- retain low-rate census/snapshot repair as an anti-entropy layer.

Exit criteria:

- event hooks reduce scans without making correctness depend on a hook firing;
- missed events are repaired by revisioned snapshots/census;
- hooks are fingerprinted and can be disabled independently.

### Phase 6 — Partition and optimize

Goal: scale only after the join path is correct.

Work:

- map world positions to stable zone/cell subscriptions;
- partition snapshot baselines and journals by interest;
- add compression and snapshot delta encoding where measured;
- reduce full census frequency after event coverage is proven.

Exit criteria:

- bandwidth/CPU measurements justify each optimization;
- crossing an interest boundary cannot orphan or duplicate an entity.

## Immediate next work

The shortest path to a decisive result is:

1. land the spike-402 evidence and keep the probe scenario-gated;
2. fix scenario verdict semantics and add session epoch/phase logging;
3. create the centralized runtime-image fingerprint/address registry;
4. perform the cross-process detached snapshot test;
5. perform disposable-character apply and double-apply tests;
6. in parallel, extract `SessionCoordinator` and `IdentityRegistry` without
   changing packet bytes;
7. only then implement checkpoint+journal cutover.

Do not spend the next iteration mapping more engine functions broadly. Discover
new functions against a specific capability needed by the live-apply or
checkpoint path.

## Explicit non-goals

- deterministic lockstep simulation;
- replacing all packets with serialized object blobs;
- whole-world live injection before object-level apply is safe;
- zone partitioning as a prerequisite for correctness;
- making clients authoritative for durable state;
- perfect support for unknown Kenshi executable builds.

## Final assessment

The project is not missing a secret "replicate world" engine call. It was
overlooking two more practical opportunities:

1. the executable used for reverse engineering must be the exact runtime
   `RE_Kenshi` image; and
2. Kenshi's existing object serialization can provide broad cold-state
   snapshots, while explicit networking remains responsible for time-sensitive
   and transactional semantics.

Those findings reduce the need for ever more hand-authored late-join field
packets, but they do not remove the need for a checkpoint boundary, stable
identity, journal ordering, and transaction conservation. The recommended
hybrid architecture turns those constraints into explicit modules and gives the
project testable stop/go gates instead of another open-ended rewrite.
