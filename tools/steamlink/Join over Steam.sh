#!/bin/sh
# Join a friend's Ocarina of Time co-op game over Steam.
cd "$(dirname "$0")" || exit 1
./OOTSteamLink join
printf 'Press Enter to close.'
read -r _
