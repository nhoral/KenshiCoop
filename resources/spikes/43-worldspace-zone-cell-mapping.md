# Spike 43 - Worldspace -> zone/cell mapping

- Type: DUMP
- Status: PARTIAL
- Save: c
- Branch commit: <filled at commit>

## Goal

Determine how a world position maps to a zone (and finer cell), whether that mapping is
reachable from the mod, and whether it is consistent across clients. This underpins
interest management ("are my peers in the same zone/cell as me?"), zone-scoped
replication, and "which region is X in" UI.

## Method

Static read of `ZoneManager.h` / `RootObject.h` for the zone surface, then a runtime DUMP:
both clients each 1.5s read their leader's zone via the public virtual
`RootObject::getZoneMapLocation()` (RVA 0x593B90) and the `GameWorld::zoneMgr` pointer
(member offset 0x8B0), logging both pointers + an `inzone` flag. Called the virtual
directly (the mod's normal pattern for virtuals) under SEH. Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 43 -Save c -Seconds 30
```

Probe code reverted; raw logs in `43/raw/`.

## The zone surface (static)

- `RootObject::getZoneMapLocation() -> ZoneMap*` (RVA 0x593B90, vtable 0xC8, `_NV_` twin)
  - the per-object "which zone am I in".
- `GameWorld::zoneMgr` (`ZoneManager*`, member 0x8B0) - the zone manager instance.
- `ZoneManager` position queries (obfuscated names, RVAs present): `getZoneMap(Vector3)`
  0x291160, `getZoneMap(int,int)` 0xA07330, `mapFromResolutionCoord(X,Z) -> iVector2`
  0xA07D30 (grid cell coords), `getZoneBounds(pos) -> AABB2D` 0xA07D60.
- `ZoneSpacialGrid` content keys (PRIVATE, RVAs present): `getZoneKey(Vector3) -> uint`
  0x9F7EE0, `getCellKey(ZoneMap*,Vector3) -> uint` 0x9F9940, `getFullKey(Vector3) -> uint`
  0x9F9A20; `cellSize` member 0x14. `getObjects(point,radius,...)` 0x9FDA60 is the spatial
  query backing interest. `ZoneMap` itself is an obfuscated/stripped type (no name field).

## Raw evidence

Every sample, both clients (`43/raw/host.log`, `43/raw/join.log`):

```
host t=13516..28516  zmgr=0x7FF471EB0018  zmap=0x7FF471F235E0  inzone=1   (all host samples)
join t=1516..28516   zmgr=0x7FF4E9120018  zmap=0x7FF4E91935E0  inzone=1   (all join samples)
```

Both `zmgr` and `zmap` were non-null and **bit-stable for the entire run** on each client.
Note: `zmap - zmgr` = **0x735C8 on both clients** (identical delta), and the low bits
match (`...018` / `...5E0`).

## Findings

1. **Worldspace -> zone mapping is reachable and works.** `getZoneMapLocation()` returned
   a non-null `ZoneMap*` for the leader on both clients every sample (`inzone=1`) - the
   per-object zone lookup is a clean public virtual the mod can call directly.
2. **The `ZoneManager` is reachable at `GameWorld+0x8B0`** and non-null on both clients,
   so the position-based queries (`getZoneMap(pos)`, `mapFromResolutionCoord`, the
   spatial-grid `getObjects`) hang off a confirmed-live instance.
3. **Zone assignment is stable for a stationary unit** - the leader's `ZoneMap*` did not
   change across ~20 samples / 30 s while parked, i.e. no zone churn at rest.
4. **Zone loading is deterministic across clients given the same save.** The
   `zmap - zmgr` offset was **identical (0x735C8)** on host and join despite different
   absolute base addresses, and the low-order bits matched. The two processes laid out
   the zone manager and the leader's zone at the same relative positions - strong evidence
   the same save deterministically produces the same zone structure on every client.

## Validation

- Findings 1-3: `43/raw/{host,join}.log`, quoted above. Non-null, stable pointers + the
  `inzone=1` flag every sample on two independent processes (role=H and role=J each emit
  their own lines). The clean self-exit (host PASS) confirms the virtual call did not
  fault under SEH.
- Finding 4: arithmetic on the logged pointers - `0x...F235E0 - 0x...EB0018 = 0x735C8`
  (host) and `0x...1935E0 - 0x...120018 = 0x735C8` (join). Identical delta is the
  evidence; it is a layout/identity hint, not a content hash (see Open questions).

## Open questions / hypotheses (UNVALIDATED)

- **No content-derived zone IDENTITY or NAME was extracted.** `ZoneMap` is obfuscated
  (no name accessor), and the comparable `uint` keys (`getZoneKey`/`getCellKey`/
  `getFullKey`) are PRIVATE on `ZoneSpacialGrid` and need the grid instance (offset
  unknown). So "are host and join in the *same named zone*" is only inferred from the
  matching pointer-offset (Finding 4), not proven by a content key. Follow-up: resolve
  `getFullKey(pos)` by raw RVA and log it on both clients for a true cross-client compare.
- **Cell-level (sub-zone grid) mapping was not exercised** - `mapFromResolutionCoord` /
  `getFullKey` (cellSize 0x14) would give the finer cell coords/key; untested.
- **Zone CROSSING was not observed** - the leader stayed parked, so transition behaviour
  (when `getZoneMapLocation` flips, load/unload timing) is unmeasured. Needs a moving or
  teleported leader.
- **`zmgr+0x8B0` offset** is taken from the header; it read sane (non-null, stable) but
  was not independently cross-checked against a second accessor.

## Implications for co-op

- Interest management can be zone-scoped now: `getZoneMapLocation()` per character gives a
  cheap "same zone?" test *within* a client; the spatial grid `getObjects(point,radius)`
  is the existing radius query.
- Zone loading being deterministic per save (Finding 4) means peers in the same area load
  the same zone structure - good for consistency - but a **content key** is still needed
  before the wire protocol can carry "peer is in zone K" reliably across machines.

## Recommended follow-ups

- Resolve `ZoneSpacialGrid::getFullKey(Vector3)` (RVA 0x9F9A20) by raw module-base+RVA
  (it is private, so member-pointer resolution won't compile) and log the `uint` on both
  clients to get a true cross-client zone/cell identity.
- Probe a moving/teleporting leader to capture a zone crossing and the transition timing.
- Use `mapFromResolutionCoord(X,Z) -> iVector2` for the cell grid coords UI.
