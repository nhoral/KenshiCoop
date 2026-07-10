KenshiCoop co-op kit
=====================================

QUICK START
-----------

Step 0 - one-time prerequisites (both players):

  1. Kenshi 1.0.65 (Steam), set to WINDOWED mode: launch Kenshi once,
     Options > Video > un-check Full Screen.
  2. RE_Kenshi 0.3.1+ installed - a free mod that loads the co-op plugin:
     https://www.nexusmods.com/kenshi/mods/847
  3. Steam RUNNING and ONLINE on both machines. That is the whole network
     setup - the connection goes through Steam, so there is no port
     forwarding, no router setup, and no IP addresses.

Step 1 - swap Steam friend codes:

  Each player needs the OTHER player's code before connecting. The
  launcher reads YOUR code from Steam and prints it on screen the moment
  you start it - just read it to the other player and type theirs in.
  (Codes are also in Steam under Friends > Add a Friend; a 17-digit
  SteamID64 works too.)

Step 2 - launch:

  One player double-clicks HOST.cmd in this folder; the other player
  double-clicks JOIN.cmd. Each asks for the other player's code, then
  does everything else: checks your setup (and tells you exactly what to
  fix if something is missing), installs the mod and the shared starter
  save, launches Kenshi, and connects the two games. When both of you
  are in-game, you're playing co-op.

  Prefer a terminal? The same flow is:
    powershell -ExecutionPolicy Bypass -File friend_host.ps1 -PeerSteamId <their code>
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <their code>

WHAT SYNCS
----------

In free play the full sync set is active by default: positions, combat,
health/limbs, stats, game speed, carried bodies, inventory + equipment
changes, items dropped on the ground, direct trades between the two
squads, and squad management (recruits you hire AND units you move
between squad tabs mid-session stay tracked on the other machine - note
they won't appear in the other player's squad UI, that's a known
limitation).

Base-building syncs too: placed buildings, construction progress, doors,
dismantling, production machines - power switches, generators, crafting
bench / furnace / drill output, input fuel and farm growth - and
container CONTENTS: every storage chest and machine inventory near the
players holds the same items on both machines.

Saves are coordinated: any save either player makes during a connected
session becomes ONE shared save - the host's game writes it and streams
the whole save folder to the other machine automatically (verified +
committed only when it arrives intact). Loading works the same way: when
either player loads a save mid-session, both games load the identical
save and the session continues from there (expect a normal load screen
on both sides).

TWO THINGS TO KNOW
------------------

  * You each control your own squad. The starter save has one squad tab
    per player: the host runs squad 1, the joining player squad 2. Your
    friend's squad is visible and synced on your screen, but answers
    only to them.
  * Both players must load the exact same save - that's why this kit
    installs the shared 'squad1' save on both machines. Don't substitute
    your own save unless you copy the identical folder to both sides.

CHOOSING A SAVE
---------------

The launcher asks which save to play (both players must pick the same):

  [1] The bundled starter save - the default. A fresh start on the
      two-squad co-op save that ships with this kit.
  [2] Your own save, picked in-game - both games start on the bundled
      save so they can connect; once BOTH players are in-game, the HOST
      opens Kenshi's menu > Load and picks any save. The other player's
      game follows automatically (the save is streamed over first if
      their copy differs). This is also how you RESUME a previous co-op
      session: any save either player made last time is already on both
      machines - just load it. Tip: the second player controls the
      save's SECOND squad tab - move some units into a new squad tab to
      give them a crew.

MANUAL INSTALL (optional)
-------------------------

The launchers do this for you, but if you prefer to install by hand: copy
the 'mod' folder to <Kenshi>\mods\KenshiCoop, and copy 'save\squad1' to
%LOCALAPPDATA%\kenshi\save\ - identically on both machines.

UNINSTALL
---------

Nothing else on your machine is touched. To remove: delete
<Kenshi>\mods\KenshiCoop and the 'squad1' save folder.

TROUBLESHOOTING
---------------

  * "Running scripts is disabled" or Windows SmartScreen blocks the kit:
    right-click the downloaded zip BEFORE extracting, Properties > Unblock.
    The HOST.cmd / JOIN.cmd launchers also bypass this automatically.
  * "RE_Kenshi not found": install RE_Kenshi into your Kenshi folder
    (https://www.nexusmods.com/kenshi/mods/847) and run the launcher again.
  * "The co-op plugin has not started": the game launched but RE_Kenshi did
    not load the plugin. Check <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop';
    reinstalling RE_Kenshi usually fixes it.
  * No connection: both Steams must be RUNNING and ONLINE (not offline
    mode), and each side must be launched with the OTHER player's code -
    a typo'd code fails silently, so re-check both. Look for
    '[steam] session ... active=1' in the log - relay=1 just means Valve
    is relaying (works fine, slightly higher ping).
  * "protocol mismatch" in the log: your kit is older/newer than the other
    player's build; both players should get the latest kit.
