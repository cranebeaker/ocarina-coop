#!/bin/sh
# Runs the Story Co-op server (port 21050) for play by address. Team saves go to server/data/story.
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")/server" || exit 1
chmod +x OOTServer 2>/dev/null
# Double-clicked in a file manager: run it in a terminal window so it can be seen and stopped
if [ ! -t 1 ] && [ -z "$OOT_IN_TERMINAL" ]; then
    export OOT_IN_TERMINAL=1
    for t in konsole gnome-terminal xfce4-terminal x-terminal-emulator xterm; do
        command -v "$t" >/dev/null 2>&1 || continue
        if [ "$t" = gnome-terminal ]; then exec "$t" -- "$SELF"; else exec "$t" -e "$SELF"; fi
    done
fi
echo "Story Co-op server on port 21050. Close this window or press Ctrl+C to stop (team progress is saved)."
./OOTServer "$@"
printf 'Server stopped. Press Enter to close.'
read -r _
