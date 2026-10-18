#!/bin/sh
# Builds out/OOTSteamLink and the mock out/libsteam_api.so (64 bit, plain C, needs gcc or clang and libdl).
# Run from any folder.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
mkdir -p "$HERE/out"
"$CC" -O2 -Wall -Wno-format-truncation -o "$HERE/out/OOTSteamLink" "$HERE/ootsteamlink.c" -ldl
"$CC" -O2 -Wall -Wno-format-truncation -fPIC -shared -o "$HERE/out/libsteam_api.so" "$HERE/mock/mock_steam_api.c"
echo "Built $HERE/out"
