#!/bin/bash
# Plays the game with diagnostics armed: optimizations can be switched off live through
# out/toggles.txt (BB_TOGGLE_FILE, re-read every 250 ms), and the output goes to a log per session.
# Usage: macos-play.sh [VAR=value ...]   (extra env for the game)
set -u
base=~/bloodborne-for-macos/base
cd "$base"
echo 0 > out/toggles.txt
log=out/play-$(date +%Y%m%d-%H%M%S).log
echo "log: $log"
env "$@" BB_TOGGLE_FILE=out/toggles.txt BB_FRAME_STATS=1 BB_GAME_DIR=../game-fixed/uroot \
    bash run.sh > "$log" 2>&1
