#!/bin/sh
# Builds the Linux / Steam Deck player tarball for Ocarina of Time Story Co-op.
#
#   sh scripts/linux/package/package.sh <soh.appimage> <OOTServer> <OOTSteamLink> <libsteam_api.so> <version> <out dir>
#
# The tarball holds the game as an AppImage (it carries its own libraries, so it runs on SteamOS and other distros
# without installing anything), the server, the Steam relay helper and the shortcuts. No ROM, no game assets: the
# game builds them from the player's own ROM on the first start. libsteam_api.so is Valve's Steamworks runtime from
# the Steamworks.NET Standalone release 2025.164.1; its SHA-256 is checked.
set -e
[ $# -eq 6 ] || { sed -n 4p "$0"; exit 2; }
APPIMAGE=$1 SERVER=$2 STEAMLINK=$3 STEAMLIB=$4 VERSION=$5 OUT=$6
STEAMLIB_SHA256=ec4797f76a206eb0af627af0f0788eb5a2eabf5dee38ee0a6affbaa44a645f4e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"

echo "$STEAMLIB_SHA256  $STEAMLIB" | sha256sum -c -

NAME="OcarinaStoryCoop-$VERSION-linux"
DIR="$OUT/$NAME"
rm -rf "$DIR"
mkdir -p "$DIR/server"
cp "$APPIMAGE" "$DIR/soh.appimage"
cp "$SERVER" "$DIR/server/OOTServer"
cp "$ROOT/server/config.json" "$DIR/server/"
cp "$STEAMLINK" "$DIR/OOTSteamLink"
cp "$STEAMLIB" "$DIR/libsteam_api.so"
printf 480 > "$DIR/steam_appid.txt"
cp "$ROOT/tools/steamlink/Host over Steam.sh" "$ROOT/tools/steamlink/Join over Steam.sh" \
   "$ROOT/scripts/linux/package/Start Server.sh" "$DIR/"
sed "s/@VERSION@/$VERSION/" "$ROOT/scripts/linux/package/README.txt" > "$DIR/README.txt"
chmod +x "$DIR/soh.appimage" "$DIR/server/OOTServer" "$DIR/OOTSteamLink" "$DIR"/*.sh

tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "Built $OUT/$NAME.tar.gz"
