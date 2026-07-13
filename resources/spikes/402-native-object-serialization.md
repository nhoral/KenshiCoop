# Spike 402 — Native object serialization and runtime address proof

Date: 2026-07-11

## Question

Can the plugin call Kenshi's native object serializers, and can a character be
captured into an isolated payload without mutating the live world's save
containers?

This spike also revisits the "base-skew" seen in spike 401. The goal is to
separate three different concerns:

1. which executable image the process actually maps;
2. whether KenshiLib resolves a callable entry point;
3. whether the serializer's behavior is useful for replication.

## Result

**PASS for isolated capture and file round-trip.**

On the host, the player leader was serialized into a probe-owned
`GameDataContainer`, saved to a temporary `.mod`, loaded into a second
container, and compared at the container/instance-count level.

Observed result:

- `GameSaveState`: 6 state records
- `INSTANCE_COLLECTION`: 1 object instance
- container: 9 `GameData` records
- payload on disk: approximately 6.7 KiB
- save: success
- load into a fresh container: success
- loaded instance and record counts matched the source
- no crash and no write into `GameWorld::savedata`

Printable fields in the resulting payload included character identity and
appearance, stats, inventory references, medical/body state, AI/state
information, and related records. This is materially broader than any one
existing KenshiCoop character packet.

This does **not** yet prove that calling `loadFromSerialise` against an existing
live character is safe or idempotent.

## Runtime image finding

The running game mapped:

`...\Kenshi\RE_Kenshi\kenshi_x64.exe`

It did not map the root-level:

`...\Kenshi\kenshi_x64.exe`

The two files are different PE images:

- root launcher/image — 36,718,592 bytes, PE timestamp `0x6602d59d`,
  `SizeOfImage=0x232d000`, SHA-256
  `a596ab4e407c67b58599c54ffb32dc1bf2b64510cdebd3fa9359ef05a576aeb1`
- `RE_Kenshi` runtime image — 36,713,984 bytes, PE timestamp `0x65d604d7`,
  `SizeOfImage=0x232c000`, SHA-256
  `504b362cde850d56afb1cea6f5b7b0ee014d9dd7b47e188599d91c804502cd3e`

Therefore the spike-401 "base skew" was not just ASLR. At least part of the
apparent mismatch came from deriving RVAs from one executable and applying them
to another. An RVA is meaningful only for the exact mapped image.

The research entry points found by scanning in spike 401 landed at stable RVAs
within the actual `RE_Kenshi` image on both processes:

- research store: `0x82e430`
- queue advance: `0x8320d0`
- research rate: `0x833680`

That makes direct RVA mapping viable for this installed build, provided it is
guarded by an exact build fingerprint and entry-point validation. Signature
scanning remains useful as a fallback and for discovering other builds; it is
not required merely because ASLR moved the module base.

## Address-only probes

At startup, when `KENSHICOOP_SPIKE=402`, the plugin logs:

- process module path and mapped base;
- `KenshiLib::GetRealAddress` results for both
  `RootObjectContainer::serialiseThings` overloads;
- `Character`, `Building`, and `Item` `serialise`/`loadFromSerialise` twins;
- `GameDataContainer::save` and `GameDataContainer::load`;
- each resolved address expressed as an RVA from the actual mapped module.

This establishes an auditable chain from SDK declaration to runtime entry
point. It does not call the probed loaders.

## Implementation

The executable spike is intentionally gated behind `KENSHICOOP_SPIKE=402`.

- `src/plugin/game/Engine.cpp`
  - emits the address map during `engine::resolve()`;
  - implements `engine::probeNativeSnapshot(GameWorld*)`;
  - owns both source and destination `GameDataContainer` instances.
- `src/plugin/game/Engine.h`
  - declares the probe.
- `src/plugin/test/Scenario.cpp`
  - invokes the probe once on the host after gameplay is available;
  - marks spike 402 passed only after the round-trip invariants hold.

## What this unlocks

Native serialization is a credible **cold-path snapshot primitive**:

- late-join character bootstrap after a world checkpoint;
- targeted resynchronization after drift;
- reconnect catch-up;
- diagnostic host/client state capture;
- potentially building and item snapshots after separate validation.

It is not a replacement for every replication mechanism:

- a 6.7 KiB character payload is unsuitable as a high-frequency transform
  stream;
- inventory moves, pickups, production, and construction still need
  conservation/transaction semantics;
- serialized references may point outside the captured object graph;
- applying a serializer to a live object may duplicate owned subobjects,
  invalidate pointers, or trigger load-only assumptions.

## Required follow-up: live-apply ladder

Do not use native payloads in production replication until these steps pass in
order:

1. Send the host payload to the join process and load it into a detached
   `GameDataContainer`; compare type, ID, record, and instance counts.
2. Apply it only to a disposable/newly created test character. Verify
   appearance, stats, medical state, inventory, ownership, and cleanup.
3. Apply the same payload twice. Counts and owned-object identities must not
   grow or change on the second application.
4. Apply to an existing remote proxy while it is out of combat and not selected.
5. Repeat with selection, inventory UI, combat, carried state, save/load, and
   zone transitions.
6. Add equivalent isolated-capture probes for `Building` and `Item`.
7. Enumerate and explicitly resolve external references. Missing references
   must fail closed or remain deferred; they must not fabricate objects.

If idempotent live apply cannot be demonstrated, retain native serialization
for checkpoint files, detached comparison, and diagnostics only.

## Decision

The plugin is not limited to the functions currently wrapped by high-level
helpers. SDK-declared entry points and manually discovered functions can both be
used safely under a common resolver:

`exact image fingerprint -> known RVA -> signature/prologue validation -> main-thread call`

Unknown builds or failed validation must disable the capability rather than
guessing an address.
