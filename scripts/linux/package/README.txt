Ocarina of Time Story Co-op @VERSION@ (Linux and Steam Deck)
==========================================================

Play the whole of Ocarina of Time with friends: one shared story, one team save, and you time travel together.
Built on Ship of Harkinian (HarbourMasters) and ShipwrightOnline (lukegrahamSydney).
Project page: https://github.com/cranebeaker/ocarina-coop

You need your own supported Ocarina of Time ROM. No game files are included.
The game is an AppImage with its own libraries: nothing to install, it runs on SteamOS and most 64-bit distros.

First start
-----------
1. Extract this folder somewhere you keep games (on a Steam Deck, in Desktop Mode, for example in your home folder).
2. Double-click soh.appimage. When it asks for your ROM, pick it (or put the .z64 file in this folder first).
   The game builds oot.o2r from it; this takes a minute and only happens once.
3. Close the game.

Play over Steam (no IP addresses, no port forwarding; Steam must be running and logged in)
-------------------------------------------------------------------------------------------
Host:  double-click "Host over Steam.sh". It starts the server, opens a friends-only Steam lobby and your game.
Join:  double-click "Join over Steam.sh". It finds a friend who is hosting and starts your game connected to them.
       With it open, right-click your friend in Steam and Join Game works too.
Then the host opens the online window, creates a party with "Story Co-op" turned on, and invites everyone.
A terminal window shows what is happening. Press Q in it (or close it) to stop; the server saves the team progress.
If your file manager opens the .sh file as text instead, right-click it and choose Run (or Run in Konsole).

Steam Deck Game Mode
--------------------
In Desktop Mode, open Steam > Games > Add a Non-Steam Game to My Library > Browse, set the file type to
"All Files", pick "Join over Steam.sh" (or "Host over Steam.sh") and add it. Rename it in its Properties if you like.
Back in Game Mode, start it from your library like any game. Errors show in a dialog and are written to
OOTSteamLink.log in this folder. Use the Steam overlay to invite friends or accept their invites.

Play by address instead
-----------------------
The host double-clicks "Start Server.sh" (port 21050; for internet play forward TCP port 21050 or use a private
network such as Tailscale or ZeroTier). Everyone starts soh.appimage and enters the host's address and port 21050 in
the online window. Use 127.0.0.1 on the host's own machine.

Everyone must use the same Story Co-op release. Servers from ShipwrightOnline (including its public server) and from
other Story Co-op releases turn the game away with "Wrong server version".

Files
-----
soh.appimage                       the game (Ship of Harkinian with ShipwrightOnline and Story Co-op)
server/OOTServer, config.json      the server; team saves go to server/data/story
OOTSteamLink, libsteam_api.so      the Steam relay helper (Valve's free "Spacewar" app, AppID 480)
