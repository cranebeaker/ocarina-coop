#!/bin/bash
# Checks the Linux tarball on a bare Arch Linux system (the base of SteamOS), as a Steam Deck would run it.
#
#   docker run --rm -v "$PWD:/w" archlinux:latest bash /w/scripts/linux/package/package_test.sh \
#       /w/<OcarinaStoryCoop-...-linux.tar.gz> /w/tools/steamlink/out/libsteam_api.so
#
# The second argument is the mock libsteam_api.so from tools/steamlink/build.sh (no Steam client in a container).
# 1 every binary finds its libraries          2 the AppImage starts with nothing installed and keeps its files
# 3 the game gets the starting environment     next to itself, even when started from another folder
#   plus OOT_SERVER (launcher, issue #1)     4 a failure without a terminal exits instead of waiting for a key
# 5 the Steam shortcut path of Host over Steam.sh runs the server and the game and writes OOTSteamLink.log
set -u
TARBALL=$1 MOCK=$2
fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }

pacman -Sy --noconfirm --needed xorg-server-xvfb xorg-xauth mesa which >/dev/null 2>&1 || { echo "pacman failed"; exit 2; }
T=$(mktemp -d)
tar -C "$T" -xzf "$TARBALL"
P=$(echo "$T"/OcarinaStoryCoop-*-linux)

# 1 libraries
for b in "$P/server/OOTServer" "$P/OOTSteamLink"; do
    missing=$(ldd "$b" 2>&1 | grep "not found")
    [ -z "$missing" ] && pass "libraries: $(basename "$b")" || fail "libraries: $(basename "$b"): $missing"
done
(cd "$T" && "$P/soh.appimage" --appimage-extract >/dev/null 2>&1)
SOH=$(ls "$T"/squashfs-root/usr/bin/soh* 2>/dev/null | head -1)
if [ -z "$SOH" ]; then
    fail "libraries: soh (AppImage did not extract)"
else
    missing=$(LD_LIBRARY_PATH="$T/squashfs-root/usr/lib" ldd "$SOH" 2>&1 | grep "not found")
    [ -z "$missing" ] && pass "libraries: soh (inside the AppImage)" || fail "libraries: soh: $missing"
fi
rm -rf "$T/squashfs-root"

# 2 start the AppImage from another folder, no FUSE in a container
before=$(ls -A "$P" | sort)
mkdir -p "$T/elsewhere" "$T/home"
(cd "$T/elsewhere" && HOME="$T/home" APPIMAGE_EXTRACT_AND_RUN=1 timeout 40 xvfb-run -a "$P/soh.appimage" \
    >"$T/soh.out" 2>&1)
code=$?
if grep -q "error while loading shared libraries\|GLIBC_[0-9.]* not found\|GLIBCXX_[0-9.]* not found" "$T/soh.out"; then
    fail "AppImage start: $(grep -m1 "error while loading\|not found" "$T/soh.out")"
elif [ $code -eq 127 ] || [ $code -eq 126 ]; then
    fail "AppImage start: exit $code"
else
    pass "AppImage start (exit $code after up to 40 s, no loader errors)"
fi
after=$(ls -A "$P" | sort)
if [ "$before" != "$after" ] && [ -z "$(ls -A "$T/elsewhere")" ]; then
    pass "AppImage keeps its files next to soh.appimage ($(comm -13 <(echo "$before") <(echo "$after") | tr '\n' ' '))"
else
    fail "AppImage files went to the start folder: elsewhere=[$(ls -A "$T/elsewhere" | tr '\n' ' ')]"
fi
tail -5 "$T/soh.out"

# test copy with the mock Steam library and a stand-in game that records its environment
M="$T/mockpkg"
cp -a "$P" "$M"
cp "$MOCK" "$M/libsteam_api.so"
cat > "$M/soh.appimage" <<'EOF'
#!/bin/sh
env > "$(dirname "$0")/game_env.txt"
pwd > "$(dirname "$0")/game_cwd.txt"
sleep 2
EOF
chmod +x "$M/soh.appimage"

# 3 launcher
(cd / && SteamAppId=777 STEAMLINK_MOCK_NAME=Host timeout 30 "$M/OOTSteamLink" host --no-server --quiet-errors \
    </dev/null >"$T/link.out" 2>&1)
if grep -q "^OOT_SERVER=127.0.0.1:21050$" "$M/game_env.txt" 2>/dev/null && grep -q "^SteamAppId=777$" "$M/game_env.txt" \
   && [ "$(cat "$M/game_cwd.txt")" = "$M" ]; then
    pass "launcher: game started in its folder with OOT_SERVER and the original SteamAppId"
else
    fail "launcher: env=[$(grep -s "OOT_SERVER\|SteamAppId" "$M/game_env.txt" | tr '\n' ' ')] cwd=[$(cat "$M/game_cwd.txt" 2>/dev/null)]"
    cat "$T/link.out"
fi
rm -f "$M/game_env.txt" "$M/game_cwd.txt"

# 4 no terminal, no Steam library: must exit, not wait for a key press
mv "$M/libsteam_api.so" "$M/libsteam_api.so.off"
timeout 10 "$M/OOTSteamLink" host </dev/null >"$T/fail.out" 2>&1
code=$?
[ $code -eq 1 ] && grep -q "libsteam_api.so not found" "$T/fail.out" && pass "error without a terminal exits (code 1)" \
    || fail "error without a terminal: exit $code: $(cat "$T/fail.out")"
mv "$M/libsteam_api.so.off" "$M/libsteam_api.so"

# 5 Steam shortcut path of the launcher script, with the real server
(cd / && SteamGameId=12345 STEAMLINK_MOCK_NAME=Host timeout 40 sh "$M/Host over Steam.sh" </dev/null >/dev/null 2>&1)
if grep -q "Server running on port 21050" "$M/OOTSteamLink.log" 2>/dev/null && [ -f "$M/game_env.txt" ] \
   && grep -q "^SteamGameId=12345$" "$M/game_env.txt"; then
    pass "Host over Steam.sh from a Steam shortcut: server, game and OOTSteamLink.log"
else
    fail "Host over Steam.sh from a Steam shortcut"
    cat "$M/OOTSteamLink.log" 2>/dev/null
fi
grep -q "Story Co-op\|Using port" "$M/server/OOTServer.log" 2>/dev/null && pass "server log written" || fail "server log missing"

echo
[ $fails -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit $fails
