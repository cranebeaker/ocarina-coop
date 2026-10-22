#!/bin/sh
# Host an Ocarina of Time co-op game over Steam (friends join with no IP or port forwarding).
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")" || exit 1
chmod +x OOTSteamLink soh.appimage server/OOTServer 2>/dev/null
# Started by Steam (a non-Steam game shortcut, for example in Steam Deck game mode): no terminal, errors show in a
# dialog and the output goes to OOTSteamLink.log
if [ -n "$SteamAppId$SteamGameId" ] && [ ! -t 1 ]; then
    exec ./OOTSteamLink host >OOTSteamLink.log 2>&1
fi
# Double-clicked in a file manager: run it in a terminal window
if [ ! -t 1 ] && [ -z "$OOT_IN_TERMINAL" ]; then
    export OOT_IN_TERMINAL=1
    for t in konsole gnome-terminal xfce4-terminal x-terminal-emulator xterm; do
        command -v "$t" >/dev/null 2>&1 || continue
        if [ "$t" = gnome-terminal ]; then exec "$t" -- "$SELF"; else exec "$t" -e "$SELF"; fi
    done
fi
./OOTSteamLink host
printf 'Press Enter to close.'
read -r _
