# KenshiCoop

Experimental **co-op multiplayer for [Kenshi](https://lofigames.com/)**, built as an
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin.

One player hosts their world; a friend connects (LAN, direct UDP, or Steam P2P)
and plays their own squad inside it. The plugin replicates squads, NPCs, combat,
inventory and equipment, direct trades between the players' squads, items
dropped on the ground (both directions), base building and container contents,
money, game speed, and more - and saves are coordinated: any save either player
makes becomes one shared save, streamed to both machines automatically.

> **Status: work in progress.** This is a hobby project under active
> development. Expect rough edges, desyncs, and crashes. Two players is the
> current design target.

## How it works

- `KenshiCoop.dll` is loaded into the game by RE_Kenshi. It hooks the engine via
  KenshiLib and drives all game mutation on the main thread.
- Networking is [ENet](https://github.com/lsalzman/enet) over UDP, with an
  optional Steam P2P tunnel (no port forwarding needed).
- The host is authoritative for the world; each client is authoritative for its
  own squad. See `docs/API_REFERENCE.md` for the full engine-control surface and
  wire protocol.

```
src/plugin/       The KenshiCoop plugin (net, sync/replication, engine facade, scenarios)
src/netproto/     Shared wire-protocol headers (plain C++03, compiled by everything)
src/nettest/      Standalone ENet console app (transport de-risking)
src/netsim/       Protocol simulator
src/prototest/    Wire-protocol unit tests
src/tunneltest/   Steam-tunnel socket-hook tests
scripts/          Build, deploy, session, and automated-test tooling (PowerShell)
docs/             Build guide + engine/API reference
third_party/      ENet patches, VC10 compat shim (deps are fetched, not committed)
```

## Try it (play with a friend)

Two players, two machines. Takes about 10 minutes the first time.

### Before you start (both players)

1. **Kenshi 1.0.65 (Steam)**, set to windowed mode: launch Kenshi once, then
   Options > Video > un-check **Full Screen**.
2. **[RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847)** installed
   (free Nexus mod - it loads the co-op plugin into the game).
3. **Steam running and online** on both machines. That's the whole network
   setup: the connection is Steam P2P, so there's no port forwarding, no
   router configuration, and no IP addresses.

### 1. Download the kit

Grab `KenshiCoop-kit.zip` from the
[latest release](https://github.com/nhoral/KenshiCoop/releases/latest) and
unzip it anywhere (both players). You do not need to clone this repository -
but if you did, the same kit is in [dist/kit](dist/kit).

### 2. Launch and swap codes

- One player double-clicks **`HOST.cmd`** in the kit folder.
- The other double-clicks **`JOIN.cmd`**.

Each launcher starts by printing **your own Steam friend code** (read
straight from Steam), so you and your friend just read your codes to each
other and type in the other player's - no digging through Steam profiles.
(A 17-digit SteamID64 works too.)

The script then does everything
else: checks your setup (and tells you exactly what to fix if something is
missing), installs the mod and the shared starter save, launches Kenshi, and
connects the two games. When both of you are in-game, you're playing co-op.

Prefer a terminal? The same flow is available as
`powershell -ExecutionPolicy Bypass -File friend_host.ps1 -PeerSteamId <code>`
(and `friend_join.ps1 -HostSteamId <code>`). Direct UDP (`-HostIp`, with port
forwarding) is also supported.

### Good to know

- **You each control your own squad.** The starter save has one squad tab per
  player: the host runs squad 1, the joining player squad 2. Your friend's
  squad is visible and synced on your screen, but answers only to them.
- **Both players must load the exact same save** - that's why the kit installs
  the shared `squad1` save on both machines. Don't substitute your own save
  unless you copy the identical folder to both sides.
- **Saving just works.** Any save either player makes during a session becomes
  one shared save on both machines. Next time, pick "Resume your last co-op
  session" at the launcher's save prompt to pick up where you left off.
- **You can play your own world.** Pick "Your own save" at the launcher's save
  prompt: the games connect on the bundled save first, then the host loads any
  save from Kenshi's menu - the other player's game follows automatically (the
  save is streamed to them if they don't have it). Give your friend a crew by
  moving units into your save's second squad tab.

### If something goes wrong

- **"Running scripts is disabled" / SmartScreen warning** - right-click the
  zip before extracting and choose Properties > Unblock, or use the `.cmd`
  launchers (they bypass this automatically).
- **"RE_Kenshi not found"** - install
  [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) into your Kenshi
  folder and re-run.
- **No connection after launch** - both Steams must be online (not offline
  mode), and each side must have entered the *other* player's code. Re-check
  the codes and re-run.
- **"protocol mismatch" in the log** - one of you has an older kit; both
  players should re-download the latest release.

The kit's `README.txt` has the full troubleshooting list, including manual
install steps.

## Building

The plugin must be compiled with the **Visual C++ 2010 (v100) x64 toolset** (a
KenshiLib requirement). Full toolchain setup, gotchas, and install steps are in
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md). Short version, once prerequisites
are in place:

```bash
cmd //c scripts/build_plugin.cmd
```

Dependencies are fetched, not committed:

- KenshiLib + precompiled libs: clone
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  into `third_party/KenshiLib_deps/`
- ENet: clone [lsalzman/enet](https://github.com/lsalzman/enet) into
  `third_party/enet/enet/` and apply the patches in `third_party/enet/patches/`
  (see `third_party/enet/README.md`)

## Development and testing

`scripts/` contains an automated two-client test harness: `dev_cycle.ps1`
rebuilds, deploys to two local installs, launches host + join, runs a named
scenario, and produces a numeric PASS/FAIL verdict from the two logs.
`regress.ps1` runs the scenario regression suite. See
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md) Parts D-E for details.

## Credits

- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) - RE_Kenshi and
  KenshiLib, which make plugins like this possible
- [lsalzman/enet](https://github.com/lsalzman/enet) - UDP networking library
- Lo-Fi Games - Kenshi

## License

[AGPL-3.0](LICENSE). KenshiLib and RE_Kenshi are GPLv3; this plugin links
KenshiLib under GPLv3 section 13 (GPL/AGPL combination). Not affiliated with
Lo-Fi Games. Non-commercial fan project.
