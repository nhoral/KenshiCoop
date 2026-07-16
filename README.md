# KenshiCoop

Setup + Demo: [https://www.youtube.com/watch?v=OqwVRRZEYGM](https://www.youtube.com/watch?v=OqwVRRZEYGM)

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

Two players, two machines. You configure the session **inside the game** with
an in-game panel (press **F2**) - you swap Steam IDs by clipboard right in the
panel, so there's no config file to edit and no launcher scripts to run. (A tiny
`coop_config.json` is only needed for LAN / direct-UDP games.)

### Before you start (both players)

1. **Kenshi 1.0.65 (Steam)**, set to windowed mode: launch Kenshi once, then
   Options > Video > un-check **Full Screen**.
2. **[RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847)** installed
   (free Nexus mod - it loads the co-op plugin into the game).
3. **Steam running and online** on both machines. That's the whole network
   setup: the connection is Steam P2P, so there's no port forwarding, no
   router configuration, and no IP addresses. (A direct-UDP mode is also
   available for LAN / port-forwarded games.)

### 1. Install the mod

Grab `KenshiCoop-kit.zip` from the
[latest release](https://github.com/nhoral/KenshiCoop/releases/latest) and
unzip it anywhere (both players). You do not need to clone this repository -
but if you did, the same kit is in [dist/mod-kit](dist/mod-kit).

The zip contains a single **`KenshiCoop`** folder. Copy that folder into your
Kenshi `mods` directory so you end up with
`<Kenshi>\mods\KenshiCoop\KenshiCoop.dll` (default Steam path:
`C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\`). Then launch
Kenshi and enable **KenshiCoop** in the Mods menu.

### 2. Connect in-game (press F2)

The Co-op panel works at the **main menu** (before you load a game) as well as
in-game, so the joining player doesn't need to load anything first.

1. Press **F2** to open the Co-op panel.
2. **Swap Steam IDs.** Each player clicks **"Copy my Steam ID"** and sends it to
   the other (Steam chat, Discord, ...). When you receive your friend's ID, copy
   it, then click **"Paste friend's Steam ID"** - the panel shows the ID it
   captured. This is per-session (nothing is written to disk), so re-paste it if
   you relaunch Kenshi.
3. Leave **Transport** on **STEAM**.
4. **Host:** load the save you want to play (or start a new game), set
   **Role: HOST**, and toggle **Connection** to **ONLINE**.
5. **Join:** straight from the **main menu** - no save needed - set
   **Role: JOIN** and toggle **Connection** to **ONLINE**. The host streams its
   world to you on connect and you load right into it. (If you already have an
   identical copy of the host's save on disk, it's used as-is instead of
   transferring.)
6. The white status line shows live state (and a banner over your leader shows
   it too, in-game). Toggle **Connection** to **OFFLINE** to leave.

**LAN / direct-UDP (advanced):** skip the Steam ID swap. Open
`<Kenshi>\mods\KenshiCoop\coop_config.json`, set `"transport": "udp"`, and put
the host's address in `"ip"` / `"port"`. Then in the panel set **Transport: UDP**
and go ONLINE. The `ip`/`port` are re-read whenever you go ONLINE, so no restart
is needed after an edit.

### Good to know

- **You each control your own squad.** With one squad tab per player, the host
  runs squad 1 and the joining player squad 2. Your friend's squad is visible
  and synced on your screen, but answers only to them. If your save has only
  one squad, move some units into a second squad tab in-game to give them a crew.
- **The joining player doesn't need the host's save.** The host picks the save
  (or starts a new game); when the join connects from the menu, the host's world
  is streamed over automatically. Already having an identical copy on disk just
  skips the transfer.
- **Saving just works.** Any save either player makes during a session becomes
  one shared save on both machines, streamed to the other side automatically.
  To resume next time, the host loads that save and goes online, and the join
  can reconnect straight from the main menu again.

### If something goes wrong

- **"The co-op plugin has not started"** - RE_Kenshi didn't load it. Check
  `<Kenshi>\RE_Kenshi_log.txt` for `KenshiCoop`; reinstalling
  [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) usually fixes it.
- **No connection (Steam)** - both Steams must be online (not offline mode), and
  each side must have **Pasted** the *other* player's ID (the panel shows the
  captured ID - confirm it matches). If "Paste friend's Steam ID" reports the
  clipboard wasn't a Steam ID, have your friend re-copy theirs. Look for
  `[steam] session ... active=1` in `<Kenshi>\KenshiCoop_*.log`.
- **"protocol mismatch" in the log** - one of you has an older build; both
  players should re-install from the same release.

The kit's `README.txt` has the full setup + troubleshooting list.

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
