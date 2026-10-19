# Ocarina of Time Story Co-op for Mac

The download includes the game and an optional co-op server for Intel and Apple Silicon Macs.
You do not need Homebrew or build tools to play.
Supply your own supported Ocarina of Time ROM. No game ROM is included.

## Install and play

1. Open OcarinaStoryCoop-mac.dmg.
2. Copy soh.app into Applications, then eject the disk image.
3. Open soh from Applications.
4. Select your supported ROM when prompted and let the game generate its assets.

This personal build is not notarized by Apple.
If macOS blocks it, try opening it once, then use System Settings > Privacy & Security > Open Anyway.

## Join a co-op game

1. Open the Zelda Online window in the game.
2. Enter the server address in Host and 21050 in Port, unless the host chose another port.
3. Choose your player name and connect.
4. Join the host's party. The host must enable Story Co-op.

Use compatible game versions across all players.
The Windows Steam launcher is not included in this Mac package.
Connect directly to the server address.

## Host on your Mac

1. Extract OcarinaStoryCoop-server-mac.zip into a folder you can keep.
2. Open Start Server.command. Keep its Terminal window open while playing.
3. In your game, connect to Host 127.0.0.1 and Port 21050.
4. Create a party and enable Story Co-op.
5. Give your friends your Mac's reachable network address and port 21050.

For internet play, the host needs TCP port forwarding or a private network connection.
Friends on the same local network can use your Mac's local network address.
Allow incoming connections if the macOS firewall asks.

Stop the server with Control-C in its Terminal window.
Keep the server folder: team saves are stored in its data/story folder.

## Build again on GitHub

In your fork, open Actions > Build Mac app > Run workflow.
After the build succeeds, download the OcarinaStoryCoop-mac artifact from the run page.
