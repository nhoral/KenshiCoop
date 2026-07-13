# Session handoff - 2026-07-13

Context capture for the next work session (likely a different model with no
memory of this one). This is the "how to be productive here in 10 minutes"
doc; it indexes the deeper docs rather than repeating them.

## What this project is

KenshiCoop: a two-player co-op mod for Kenshi (2018, 32/64-bit OGRE engine,
no modding API for netcode). It works by DLL injection via RE_Kenshi +
KenshiLib hooks: one client HOSTS (authoritative for world NPCs), the other
JOINS (drives local copies of whatever the host streams). Both load the same
save. Wire protocol is custom (ENet UDP + Steam P2P fallback), defined in
`src/netproto/Wire.h`.

- Plugin source: `src/plugin/` (builds `KenshiCoop.dll`, MSVC v100 toolset,
  explicit file list in `src/plugin/KenshiCoop.vcxproj` - new .cpp files must
  be added there by hand).
- Unit tests: `src/prototest/` -> `dist/prototest.exe` (227 checks, wire
  structs + interp).
- Validation: in-game scripted scenarios (`src/plugin/test/Scenario*.cpp`)
  judged by PowerShell oracles (`scripts/CoopOracles.psm1` +
  `scripts/oracles/*.ps1`) over the plugin's log output. **Log strings are
  frozen API** between the C++ and the oracles - never rephrase one.

## Where things stand (end of this session)

All work is committed on `main` (local; ahead of origin - push when the user
asks). Recent history, newest first:

- `d6d2f66`..`429c5ae` - **the monolith split** (2026-07-12/13): Engine.cpp,
  Replicator.cpp, Scenario.cpp, CoopOracles.psm1 mechanically decomposed into
  domain files with zero behavior/wire changes. Per-stage commits, each gated
  by build + prototest + smoke scenarios. Full-tier regression (83 runs) run
  overnight and recorded in VALIDATION_BASELINE.md - 5 environmental fails
  reproduced identically on a pre-split control build, so the split is clean.
- `1242ffc` - join-initiated town combat fix (canonical-hand capture
  translation, `[combat] CAP xlate`; repro save `no-fight`; new
  `assault_town` scenario). Manually validated.
- `ae44f45` - protocol 39: unified entity lifecycle + creature-size (age)
  sync (join-side giant animals fix). Manually validated.
- `9cb0dd4` - Phase 1+2 NPC tier management: far minting at census discovery
  (~2000 u instead of 600 u pop-in), 2 Hz mid-band streaming, anti-zombie
  drive. Manually validated ("saw enemies far away, no zombie NPCs").
- `9401208` / `5613f11` - join-crash hardening, pack-hidden fix, research
  tech-tree sync, NPC pop-out + rubber-banding fixes.

The codebase layout after the split is documented in
**`resources/CODE_MAP.md`** - file inventories per plane, shared-state
contracts (who writes/reads each hub + invariants), and the log-tag index
(emitter -> consuming oracle). Read it before touching Engine*/Replicator*/
Scenario* or the oracles.

## Doc index

| Doc | What it holds |
|---|---|
| `CODE_MAP.md` | post-split module map, shared-state contracts, log-tag index |
| `VALIDATION_BASELINE.md` | every feature's validation record, known flakes, the 2026-07-13 full-tier run |
| `ARCHITECTURE_REVIEW_2026-07-11.md` | the deeper re-architecture proposal (SessionCoordinator, IdentityRegistry, channel objects, checkpoint+journal join) - NOT yet done; the monolith split was its documented prerequisite |
| `INTENT_REPLICATION.md`, `SYNC_GAPS.md`, `MASTER_PLAN.md` | design rationale and open gaps |
| `BUILD_SETUP.md` | toolchain/bootstrap details |

## Daily workflow (commands)

Shell is Git Bash on Windows - quote cmd paths and escape `$` for PowerShell
(both bit this session; see gotchas).

Build the plugin:

```bash
cmd //c 'scripts\build_plugin.cmd'
```

Unit tests:

```bash
cmd //c 'scripts\build_prototest.cmd'
```

```bash
./dist/prototest.exe
```

Deploy (copies the DLL to BOTH installs - host Steam copy and the join copy
at `C:\Users\mikez\Kenshi-Join`; always deploy before live testing):

```bash
cmd //c 'scripts\deploy.cmd'
```

One live scenario (launches two game instances, runs the scripted scenario,
judges, writes `tools/test-runs/<stamp>/verdict.json` + screenshots):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario coop_presence
```

Smoke set used to gate every change: `coop_presence`, `npc_sync`,
`player_combat`, `spawn_far` (~2.5 min per run).

Regression tiers (smoke ~5 runs; full = 83 runs, ~3.5 h):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/regress.ps1 -Tier smoke
```

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/regress.ps1 -Tier full
```

Offline re-judge of an existing run dir (oracle changes are validated this
way - byte-compare verdict.json before/after, excluding `timestamp`):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/analyze_run.ps1 -RunDir tools/test-runs/<stamp> -Scenario <name>
```

Manual free-play sessions: the user validates on an ultrawide (3440x1440,
two 1720x1440 windows side by side); automated runs use the laptop monitor
(2560x1600). `scripts/manual_session.ps1` / `host_session.ps1` /
`join_session.ps1` launch these. Saves used for testing: `squad1` (2-tab
squad), `sync` (bar full of armed NPCs), `duel1`, `no-fight` (town-combat
repro), `coopresume` (save/load scenarios).

## Known flakes (do NOT chase these as regressions)

- `npc_sync` **smoothness** gate: clock-slew flake. Signature: huge
  `slewSkip` (thousands), low `active`, zeroFrac inflated. Retry passes.
  Documented in VALIDATION_BASELINE.md ("known flake, kept visible").
- `body_state SKIP - no/too few host-down samples` on npc_sync: normal (no
  NPC happened to go down); SKIP not FAIL.
- `npc_carry` availability flake: "no host NPC latch" when wandering world
  NPCs drift out of interest range on save `sync`.
- `march`/`smoothness` advisory FAILs during combat-heavy runs: known noise.
- Under heavy machine load (e.g. during the 83-run marathon), `craft_order`,
  `recruit_sync`, `spawn_sync` on save `sync` can fail environmentally - the
  2026-07-13 baseline entry shows the control-build technique for
  dispositioning such failures (rebuild a known-good commit, same night,
  compare signatures).
- `regress.ps1` has a retry-once flake policy; retry-pass is recorded FLAKY
  and counts as pass.

## Conventions and gotchas

- **Mechanical-change discipline**: the user gates work in stages with manual
  validation between them, and wants commits per logical change (bisectable).
  Plans are drafted as plan files with todos; do not edit the plan file.
- **Wire/protocol changes**: never mix with refactors. Public headers
  (`Engine.h`, `Replicator.h`, `Scenario.h`) are stable surfaces.
- Adding a scenario: class goes TU-private in its `Scenario<Domain>.cpp`,
  registered in that TU's `make<Domain>Scenario`; wire gates in
  `scripts/scenarios.psd1`. Adding an oracle: function in its
  `scripts/oracles/<Domain>.ps1` fragment, dispatch entry in
  `Invoke-OneOracle`, export in the root psm1's `Export-ModuleMember`.
- New .cpp files: add `<ClCompile>` to `KenshiCoop.vcxproj` (explicit list).
- `ReplicatorUtil.h` / `ScenarioSupport.h` are PRIVATE shared preludes; no
  file-scope mutable state in ReplicatorUtil.h (cross-TU state = class
  member). Helpers shared across Engine TUs get external linkage +
  declaration in `EngineInternal.h` - never duplicated.
- Git Bash quirks: run cmd scripts as `cmd //c 'scripts\foo.cmd'`; inline
  PowerShell needs `\$var` escaping (unescaped `$s` gets eaten by bash - a
  foreach loop silently lost its variable this session).
- `verdict.json` files may be UTF-16 or BOM'd UTF-8 - parse with an encoding
  fallback (utf-8-sig / utf-16 / utf-8).
- `run_test.ps1` exit code follows the verdict (FAIL run => exit 1) - not a
  harness error.
- The stale-DLL trap: verdicts have been produced against an old DLL before;
  the build-stamp log line exists to catch it. Deploy after every build.
- `tools/test-runs/history.jsonl` accumulates per-gate metrics;
  `scripts/report_history.ps1` trends them (repeat flakes surface there).

## Sensible next steps (not started)

- Push the five split commits to origin (user usually asks explicitly).
- The ARCHITECTURE_REVIEW re-architecture (SessionCoordinator,
  IdentityRegistry, channel objects, checkpoint+journal join) is now
  unblocked by the split, if the user wants to pursue it.
- Open sync gaps live in `SYNC_GAPS.md`; the user drives priorities from
  manual free-play observations, so expect the next task to be a bug report
  from a play session.
