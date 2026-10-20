#!/bin/bash
# Unattended crash test: launches the game, taps Cross through the title and main menu until a
# full scene renders (Continue loads the save), then idles until a crash or the time limit.
# Usage: autorun.sh <log> <idle-seconds> [VAR=value ...]   (extra env for the game)
set -u
log=$1; idle=$2; shift 2
base=~/bloodborne-for-macos/base
pad=${TMPDIR:-/tmp}/bb-autorun-pad.txt
: > "$pad"
cd "$base"
env "$@" BB_PAD_FILE="$pad" BB_FRAME_STATS=1 BB_GAME_DIR=../game-fixed/uroot bash run.sh > "$log" 2>&1 &
runner=$!
start=$(date +%s)
ingame=0
tap() { echo cross > "$pad"; sleep 0.3; : > "$pad"; }
while kill -0 $runner 2>/dev/null; do
    now=$(date +%s)
    if [ $ingame = 0 ]; then
        draws=$(grep "^Frame stats" "$log" | tail -1 | sed -nE 's/.* ([0-9]+) draws\/frame.*/\1/p')
        if [ -n "$draws" ] && [ "$draws" -gt 300 ]; then
            ingame=$now; echo "in game after $((now-start)) s ($draws draws/frame)"
        elif grep -q "splash screen hidden" "$log" && [ $((now-start)) -lt 240 ]; then
            tap; sleep 2.5
            continue
        fi
    elif [ $((now-ingame)) -ge "$idle" ]; then
        echo "no crash after $idle s in game"; pkill -x bb-probe; wait $runner; exit 0
    fi
    sleep 2
done
wait $runner; code=$?
echo "game exited (code $code) after $(( $(date +%s)-start )) s, in game since $([ $ingame = 0 ] && echo never || echo $((ingame-start)) s)"
grep -m1 -E "mvk-error|Device lost" "$log"
exit 1
