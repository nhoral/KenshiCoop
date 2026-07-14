KenshiCoop - co-op mod
======================

This is just the mod. Both players install it, then set everything up inside
the game with the in-game panel (F2). Nothing is baked in - no IDs, no roles.

PREREQUISITES (both players)
----------------------------
  1. Kenshi 1.0.65 (Steam), set to WINDOWED mode:
     launch Kenshi once, Options > Video > un-check Full Screen.
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. For the Steam transport (recommended): Steam RUNNING and ONLINE on both
     machines. No port forwarding, no IPs. Each player just needs the OTHER
     player's Steam ID (Steam > Friends > Add a Friend shows a friend code; a
     17-digit SteamID64 also works).

INSTALL
-------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Double-click INSTALL.cmd. It copies the mod into
     <Kenshi>\mods\KenshiCoop.
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

SET THE FRIEND CODE (once, in the config file)
----------------------------------------------
  Open <Kenshi>\mods\KenshiCoop\coop_config.json in Notepad and set:
    "steamPeer" : the OTHER player's 17-digit SteamID64 (keep the quotes).
  For a direct-UDP game instead of Steam, set "transport": "udp" and put the
  host's address in "ip" (and "port" if you changed it).

  Don't know your friend's ID? Launch Kenshi, press F2, click "Copy my Steam ID",
  and paste it to each other - then each of you puts the other's ID in steamPeer.

  You can edit this file any time; the panel re-reads it whenever you go ONLINE
  (toggle Connection OFF then ON), so no restart is needed.

PLAY
----
  1. Both players LOAD THE SAME SAVE. (Co-op resolves units by identity, so both
     games must start from an identical save. Pick any save you both have, or
     share one save file first.)
  2. Press F2 to open the Co-op panel.
  3. One player sets Role: HOST, the other sets Role: JOIN (click the button to
     toggle).
  4. Set Transport (STEAM recommended - must match steamPeer/udp above).
  5. Toggle Connection to ONLINE. The white status line shows live state (and a
     banner over your leader shows it too). Toggle to OFFLINE to leave.

The panel shows the friend code loaded from coop_config.json (read-only) and
your own Steam ID, with a "Copy my Steam ID" button to share it.

UNINSTALL
---------
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (Steam): both Steams must be RUNNING and ONLINE, and each
    side's coop_config.json steamPeer must be the OTHER player's ID (a typo
    fails silently). Look for '[steam] session ... active=1' in
    <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same kit.
