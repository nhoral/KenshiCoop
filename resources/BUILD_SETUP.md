# Build Setup

This document describes how to build the two artifacts in this repo:

1. `KenshiCoop.dll` - the in-game KenshiLib / RE_Kenshi plugin (`src/plugin`). Must
   be built with the legacy Kenshi toolchain.
2. `nettest` - a standalone ENet console app (`src/nettest`) used to de-risk the
   networking layer. Built with any modern compiler / CMake.

The shared packet definitions in `src/netproto/Protocol.h` are compiled by both,
so that header is intentionally plain C++03 (no modern STL).

---

## Part A - Plugin toolchain (the finicky one)

KenshiLib plugins MUST be compiled with the Visual C++ 2010 (v100) x64 compiler.
This mirrors the requirements of `BFrizzleFoShizzle/KenshiLib_Examples`, whose
`.vcxproj` files declare `<PlatformToolset>v100</PlatformToolset>` and target the
Windows 10 SDK (`WindowsTargetPlatformVersion=10.0`).

### Prerequisites
Status legend: [DONE] = fetched/configured in this repo automatically; [MANUAL] =
you must install it (interactive Microsoft installer or authenticated download).

- [DONE] KenshiLib + precompiled deps. Cloned (git-LFS) from
  `https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps` into
  `third_party/KenshiLib_deps/` (git-ignored). Provides KenshiLib `Include/`,
  `Libraries/KenshiLib.lib`, `OgreMain_x64.lib`, `MyGUIEngine_x64.lib`.
- [DONE] Boost 1.60.0. Bundled inside the deps repo and extracted to
  `third_party/KenshiLib_deps/boost_1_60_0/` (contains the `boost/` headers).
- [DONE] Environment variables (`KENSHILIB_DIR`, `BOOST_INCLUDE_PATH`,
  `BOOST_ROOT`, `KENSHILIB_DEPS_DIR`) set at user scope pointing at the above.
  These apply to newly launched shells/IDE sessions.
- [MANUAL] The Visual C++ 2010 (v100) x64 toolset. Not available via winget
  (only the 2010 *runtime* redistributables are). Install Visual Studio 2010
  (any edition; Express was free), OR the Windows SDK 7.1 + "VC++ 2010 SP1
  Compiler Update for the Windows SDK 7.1" which ships the v100 x64 compiler.
  Modern MSBuild / VS2019+ can then drive the v100 toolset.
- [MANUAL] Visual Studio 2019 or newer (or VS2022 Build Tools) for MSBuild. The
  v100 toolset above is invoked through it.
- [MANUAL] A separate Windows 7.1 SDK is NOT required - the example projects
  target the Windows 10 SDK. You only need the Win7.1 SDK if you take the
  "SDK 7.1 + compiler update" route above to obtain the v100 compiler itself.
- [MANUAL] RE_Kenshi 0.3.1+ installed in your Kenshi install
  (`https://www.nexusmods.com/kenshi/mods/847`). Requires a Nexus account /
  Kenshi ownership, so it can't be fetched non-interactively.

If you get stuck, the community walkthrough at
`https://github.com/weisspure/re_kenshi-working-solution/blob/main/README.md`
is the most reliable end-to-end setup guide.

### Environment variables expected by the project (already set)
- `KENSHILIB_DIR` -> `third_party/KenshiLib_deps/KenshiLib` (contains `Include/`
  and `Libraries/`). `KenshiCoop.vcxproj` adds `$(KENSHILIB_DIR)/Include` to
  includes and `$(KENSHILIB_DIR)/Libraries/` to the linker search path, and links
  `kenshilib.lib`.
- `BOOST_INCLUDE_PATH` -> `third_party/KenshiLib_deps/boost_1_60_0`.

### Build
From a fresh `bash`/`cmd` (so the env vars above are loaded), run the tracked
helper, which feeds MSBuild a complete `PATH`/`INCLUDE`/`LIB` and uses
`UseEnv=true` (so it does not depend on VS2010 registry auto-detection):

```bash
cmd //c scripts/build_plugin.cmd
```

Output: `src/plugin/x64/Release/KenshiCoop.dll` (x64 DLL exporting `startPlugin`).

You can also open `KenshiCoop.sln` in the VS2022 IDE and build **Release | x64**,
but the IDE relies on the registry fix below.

#### Gotchas this toolchain hits (all already handled in-repo)
1. **`cl.exe` needs the 2010 runtime.** The v100 compiler depends on
   `msvcr100.dll`; keep the VC++ 2010 x86+x64 redistributables installed.
2. **MSB8003 / `cl.exe` exit `-1073741515`.** On a no-full-VS2010 machine MSBuild
   can't build the compiler's DLL search path. Fixes: add registry key
   `HKLM\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\SxS\VS7` value `10.0` =
   `C:\Program Files (x86)\Microsoft Visual Studio 10.0\`, and/or use the
   `UseEnv=true` script above (which sidesteps it entirely).
3. **Missing `ammintrin.h`.** The KB2519277 compiler update ships an `intrin.h`
   that includes `ammintrin.h` without providing it. Shim:
   `third_party/vc10_compat/ammintrin.h` (added to `INCLUDE`).
4. **ENet 1.3.18 C99 `for`-decls.** The v100 C compiler is C89-only; apply
   `third_party/enet/patches/0001-enet-c89-for-loops.patch` after fetching ENet.
5. **Ogre symbols.** `Ogre::Quaternion::getYaw` is imported from Ogre, so the
   project links `OgreMain_x64.lib` (from the deps) in addition to `kenshilib.lib`.

### Install / run
RE_Kenshi loads plugins listed by a `RE_Kenshi.json` placed in a normal Kenshi mod
folder. Lay it out like the HelloWorld example:

```
[Kenshi install dir]/mods/KenshiCoop/
    KenshiCoop.dll        <- copied from x64/Release/
    RE_Kenshi.json        <- from dist/mods/KenshiCoop/RE_Kenshi.json
    KenshiCoop.mod        <- empty FCS mod template (see note below)
```

`RE_Kenshi.json` content (already provided in `dist/mods/KenshiCoop/`):
```json
{ "Plugins" : [ "KenshiCoop.dll" ] }
```

Note on the `.mod` file: Kenshi only shows a mod in the **Mods** tab if the folder
contains a `.mod` file. The `.mod` is a small binary FCS file that cannot be
hand-written as text here. Create an empty one once via the FCS ("New" -> name it
`KenshiCoop` -> save), or copy any example mod's `.mod` and rename it. Place it
next to the DLL.

Then launch via RE_Kenshi and enable **KenshiCoop** in Kenshi's Mods tab. On load
you should see `KenshiCoop loaded!` in RE_Kenshi's debug log (Milestone 1).

---

## Part B - nettest standalone app

This does not need the Kenshi toolchain. It only needs ENet (vendored under
`third_party/enet`, see that folder's README) and CMake.

```bash
cmake -S src/nettest -B build/nettest
cmake --build build/nettest --config Release
```

Run two instances on loopback (Milestone 3):
```bash
# terminal 1 (host)
nettest host 27800

# terminal 2 (client)
nettest join 127.0.0.1 27800
```

You should see PlayerStatePackets being exchanged and printed on both sides.

---

## Part C - Running two clients in-game (Milestone 5)

The plugin chooses host vs join from environment variables read at load time, so
no recompile is needed to switch roles:

- `KENSHICOOP_MODE` = `host` (default) or `join`
- `KENSHICOOP_IP`   = host's IP address when joining (default `127.0.0.1`)
- `KENSHICOOP_PORT` = UDP port (default `27800`)
- `KENSHICOOP_SAVE` = name of a save to auto-load on the title screen (default
  empty = pick a save manually from the menu). This must be an **existing save
  folder name** under `%LOCALAPPDATA%\kenshi\save\<name>` (e.g. `c`). Auto-loading
  a save that does not exist will crash the game, so pass a real name (the test
  runner validates this for you).
- `KENSHICOOP_AUTOLOAD_DELAY_MS` = how long to let the main menu settle before
  issuing the auto-load (default `5000`). The deferred load crashes if triggered
  before the menu/save subsystem is ready; the default is a safe margin.
- `KENSHICOOP_TEST_SECONDS` = if > 0, the plugin self-exits this many seconds
  after gameplay starts (default `0` = never). Used by the automated test runner;
  leave unset for normal play.
- `KENSHICOOP_LOG` = path to the dedicated, per-line-flushed KenshiCoop log
  (default `KenshiCoop_host.log` / `KenshiCoop_join.log` in the working dir). This
  log is machine-readable (timestamp + `[HOST]`/`[JOIN]` tag per line) and is
  written in addition to RE_Kenshi's `kenshi.log`.

Because two Kenshi instances on one machine is impractical (single-instance and
save locking), use two machines (or one machine + a VM) on the same LAN.

Machine A (host) - set before launching Kenshi via RE_Kenshi:
```bash
set KENSHICOOP_MODE=host
set KENSHICOOP_PORT=27800
```

Machine B (join):
```bash
set KENSHICOOP_MODE=join
set KENSHICOOP_IP=192.168.1.50
set KENSHICOOP_PORT=27800
```

Load a save on both. As you move your character on machine A, machine B's
RE_Kenshi debug log should print updating `remote player ... pos (...)` lines, and
vice-versa. Net-thread diagnostics (connect/disconnect) are emitted via
`OutputDebugStringA` and are visible in DebugView or a debugger's output window.

Open UDP `KENSHICOOP_PORT` on the host's firewall.

### Auto-loading a shared save (convenience)

Set `KENSHICOOP_SAVE` to the same save name in both `scripts/launch_host.cmd` and
`scripts/launch_join.cmd` so both clients boot straight into the shared world. Run
`scripts/sync_save.cmd` first so the join install has that save on disk.

---

## Part D - Automated test runner (single machine)

For unattended runs (e.g. a Cursor session validating a change), the test runner
launches both clients on one machine, auto-loads a save, lets them run for a fixed
time, self-exits, and collects per-client logs and screenshots.

Prereqs (once): build + `scripts/deploy.cmd`, `scripts/setup_join_install.cmd`,
and a save that exists in the host install.

### One-shot autonomous cycle (`scripts/dev_cycle.ps1`)

The single command an unattended/Cursor session should use each iteration. It
kills any stale Kenshi, rebuilds the plugin, redeploys to both installs, runs the
host+join test, and **exits 0 on PASS / non-zero on FAIL or build error** - so an
agent can branch on the exit code without parsing output:

```bash
powershell -ExecutionPolicy Bypass -File scripts/dev_cycle.ps1 -Save "c" -Seconds 60 -Sync
```

Use `-SkipBuild` when only the test/scripts changed. Logs + screenshots land under
`tools/test-runs/<stamp>/` (path printed at the end).

### Just the test (`scripts/run_test.ps1`)

```bash
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Save "c" -Seconds 60 -Sync
```

or the thin wrapper:

```bash
scripts\run_test.cmd "MyCoopSave" 60
```

What it does:
- Validates `-Save` exists under `%LOCALAPPDATA%\kenshi\save\` first (fails fast
  with the list of available saves if not - this is what catches a typo'd name).
- `-Sync` mirrors host saves into the join install (so both load the same save).
- Launches host (Steam install) then join (`%USERPROFILE%\Kenshi-Join`), each with
  its own `KENSHICOOP_LOG`, `KENSHICOOP_SAVE`, and `KENSHICOOP_TEST_SECONDS`.
- Gets each instance **past Kenshi's launcher** automatically (see below), then
  tracks the real game process for screenshots / self-exit.
- Kills any stale Kenshi processes first (so a crashed prior run can't hold the
  DLL lock or confuse process detection). Pass `-NoKill` to skip.
- Waits until the host log shows `gameplay started`, then captures a screenshot of
  each window while both are still in-game.
- Waits for both to self-exit (hard-timeout kill as a safety net).
- Prints a per-client `PASS`/`FAIL` and a final `RESULT: PASS|FAIL`, and **exits 0
  on PASS / 1 on FAIL**. A client fails if it never reached gameplay, didn't exit
  cleanly via the self-exit timer (i.e. crashed), or logged any `ERROR:` lines.

Outputs land in a timestamped folder under `tools/test-runs/<stamp>/`:
`host.log`, `join.log`, `host.png`, `join.png`.

### Getting past Kenshi's launcher (`scripts/start_kenshi.ps1`)

Kenshi shows a Win32 launcher (Video settings / Mods / Changelog, with an `OK`
button) and blocks there until `OK` is clicked - RE_Kenshi and our plugin only
load *after* that click. Also, the `kenshi_x64.exe` you launch is just a loader
that relaunches the real game as a separate process named **`Kenshi_x64`** (a
different PID). `start_kenshi.ps1` handles both: it launches the loader, finds the
new game window, clicks `OK` via `BM_CLICK` (cross-process, no screen
coordinates), and returns the real game PID. The test runner uses it for both
clients; you can also use it standalone:

```bash
powershell -ExecutionPolicy Bypass -File scripts/start_kenshi.ps1
```

### Ad-hoc screenshots

`scripts/screenshot.ps1` captures any window by PID (the reliable discriminator,
since both clients run as `Kenshi_x64`) using Win32 `PrintWindow` with a `BitBlt`
fallback, so it works even when the window is unfocused or overlapped:

```bash
powershell -ExecutionPolicy Bypass -File scripts/screenshot.ps1 -ProcessId 12345 -Out shot.png
```

Note: run the game in windowed/borderless (not exclusive-fullscreen) mode so
capture is reliable.

