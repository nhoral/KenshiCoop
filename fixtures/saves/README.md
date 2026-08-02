# Repo-tracked test saves (fixtures)

These are the pristine validation saves the functional harness loads. Each
subfolder is a full Kenshi save (`quick.save`, `platoon/`, `zone/`,
`portraits_texture.png`) matching the layout of `%LOCALAPPDATA%\kenshi\save\<name>`.

## Why they live here

Both the host and `Kenshi-Join` installs load saves from the SAME per-user folder
(`%LOCALAPPDATA%\kenshi\save`). On connect, the host `armConnectPush()` bakes the
live world over the loaded save, and the join stream-commits into the same folder,
so a validation fixture would silently rot across runs ("fixture drift"). Keeping
the source of truth in the repo and restoring it before every run means the game
only ever mutates the throwaway AppData copy; drift is discarded each run.

## Workflow

- **Restore (repo -> AppData):** `scripts/deploy_saves.ps1 -Save <name>` (or
  `-All`). `scripts/run_test.ps1` calls this automatically for the scenario's save,
  so `regress.ps1` is self-healing with no extra steps.
- **Update a fixture (AppData -> repo):** the ONLY sanctioned way to change a
  fixture is `scripts/capture_save.ps1 -Save <name>` after a deliberate edit/bake,
  or `scripts/bake_scene.ps1 ... -Promote` which captures the freshly baked save.
  Review the resulting `git diff` before committing.

## Saves tracked here

Validation fixtures referenced by `scripts/scenarios.psd1`:
`sync`, `squad1`, `c`, `duel1`, `down1`, `craft1`, `bedcage1`, `pole1`, `camp`,
`jailed`, `coopresume`.

Three-player fixtures (protocol 49, `scripts/run_3p_smoke.ps1`): `squad3` - the
squad1 squad split into THREE tabs (ranks 0/1/2 = host / join 1 / join 2, one
member each; baked via `bake_scene.ps1 -Setup squad3 -BaseSave squad1
-BakeSave squad3 -Promote`).

Debug/exploratory saves (`together`, `separate`, `zoom`, ...) are intentionally NOT
tracked - drift there is harmless. See the `coop-save-orchestration` skill for the
full catalog and each save's purpose.
