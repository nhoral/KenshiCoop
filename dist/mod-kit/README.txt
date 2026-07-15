KenshiCoop - co-op mod
======================

This zip contains ONE folder: "KenshiCoop". That folder IS the mod.

INSTALL (both players)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Copy the "KenshiCoop" folder into your Kenshi mods folder:
       <Kenshi>\mods\
     so you end up with:
       <Kenshi>\mods\KenshiCoop\KenshiCoop.dll   (and the other files)
     The default Steam path is:
       C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

PREREQUISITES (both players)
----------------------------
  1. Kenshi 1.0.65 (Steam), set to WINDOWED mode:
     launch Kenshi once, Options > Video > un-check Full Screen.
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. For the Steam transport (recommended): Steam RUNNING and ONLINE on both
     machines. No port forwarding, no IPs, no config editing - you swap Steam
     IDs in-game (see PLAY below).

PLAY (Steam - recommended)
--------------------------
  1. Press F2 to open the Co-op panel. It works at the MAIN MENU (before loading
     a game) as well as in-game.
  2. Swap Steam IDs: each player clicks "Copy my Steam ID" and sends it to the
     other (Steam chat, Discord, etc.). When you receive your friend's ID, copy
     it, then click "Paste friend's Steam ID" in your panel. The panel shows the
     ID it captured. (This is per-session - re-paste it if you relaunch Kenshi.)
  3. HOST: load the save you want to play (or start a new game), set Role: HOST,
     leave Transport on STEAM, and toggle Connection to ONLINE.
  4. JOIN: straight from the MAIN MENU - no save needed - set Role: JOIN, leave
     Transport on STEAM, and toggle Connection to ONLINE. The host sends its
     world to you on connect and you load right into it. (You do NOT need the
     host's save beforehand. If you already have an identical copy on disk it is
     used as-is instead of transferring.)
  5. The white status line shows live state (and a banner over your leader shows
     it too, in-game). Toggle Connection to OFFLINE to leave.

PLAY (LAN / direct UDP - advanced)
----------------------------------
  Skip the Steam ID swap. Open <Kenshi>\mods\KenshiCoop\coop_config.json in
  Notepad, set "transport": "udp", and put the host's address in "ip" (and
  "port" if you changed it). In the panel set Transport: UDP, pick Host/Join,
  and go ONLINE. ip/port are re-read whenever you go ONLINE, so no restart is
  needed after an edit.

UNINSTALL
---------
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (Steam): both Steams must be RUNNING and ONLINE, and each side
    must have Pasted the OTHER player's ID (the panel shows the captured ID -
    confirm it matches). If "Paste friend's Steam ID" says the clipboard wasn't
    a Steam ID, have your friend re-copy theirs with "Copy my Steam ID". Look for
    '[steam] session ... active=1' in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same release.
