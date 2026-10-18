#!/bin/sh
# Host an Ocarina of Time co-op game over Steam (friends join with no IP or port forwarding).
cd "$(dirname "$0")" || exit 1
./OOTSteamLink host
printf 'Press Enter to close.'
read -r _
