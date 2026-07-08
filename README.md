# KenshiCoop

Experimental **co-op multiplayer for [Kenshi](https://lofigames.com/)**, built as an
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin.

One player hosts their world; a friend connects (LAN, direct UDP, or Steam P2P)
and plays their own squad inside it. The plugin replicates squads, NPCs, combat,
inventory, world items, money, game speed, and more between the two clients.

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

You need: Kenshi (Steam) and [RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847)
on both machines. Everything else is bundled in [dist/kit](dist/kit): the
prebuilt mod, a shared two-squad starter save (`squad1`), and one-command
host/join scripts that install both for you.

First, exchange Steam IDs - each player needs the *other* player's ID before
launching. Any of these work (the scripts accept either form):

- **Steam friend code** (shortest): in Steam, go to Friends > Add a Friend -
  your friend code is shown at the top. Swap codes with your friend.
- **SteamID64** (17-digit number): in Steam, click your profile name > Account
  details - your Steam ID is shown under your account name. Or paste your
  profile URL into [steamid.io](https://steamid.io) and use the `steamID64`
  value.
- Already launched? The host's ID is also printed in the console after the game
  starts (`>>> YOUR SteamID: ...`), ready to read to the joining player.

Then, on each machine, from the `dist/kit` folder:

Host:

```powershell
powershell -ExecutionPolicy Bypass -File friend_host.ps1 -PeerSteamId <their id>
```

Join:

```powershell
powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <their id>
```

The scripts copy the mod into `[Kenshi]/mods/KenshiCoop`, install the `squad1`
save, launch the game, and connect via Steam P2P (no port forwarding). Direct
UDP (`-HostIp`, with port forwarding) is also supported; the scripts print
step-by-step guidance as they run.

Two things to know before playing:

- **Both players must load the exact same save.** Entity identity is resolved
  from the save itself, so host and join have to start from identical copies -
  that's why the kit ships `squad1` and installs it on both machines. Don't mix
  it with your own saves unless you copy the same save folder to both sides.
- **You only control your own squad tab.** The shared save's player faction has
  one squad tab per player: the host controls squad tab 1, the joining player
  controls squad tab 2. Your friend's squad is visible and fully synced on your
  screen, but its characters answer only to them (and vice versa).

Prefer a manual install? Copy [dist/mods/KenshiCoop](dist/mods/KenshiCoop) into
`[Kenshi]/mods/` and `dist/kit/save/squad1` into `%LOCALAPPDATA%\kenshi\save\`
on both machines - the kit scripts do exactly this.

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
