#!/bin/bash
# Unattended crash test: launches the game, taps Cross through the title and main menu until a
# full scene renders (Continue loads the save), then idles until a crash or the time limit.
# Usage: BB_GAME_DIR=<game> scripts/macos-autorun.sh <log> <idle-seconds> [VAR=value ...]
set -u
log=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1"); idle=$2; shift 2
base=$(cd -- "$(dirname -- "$0")/.." && pwd)
game=${BB_GAME_DIR:?set BB_GAME_DIR to the game folder}
pad=${TMPDIR:-/tmp}/bb-autorun-pad.txt
: > "$pad"
cd "$base"
env "$@" BB_PAD_FILE="$pad" BB_FRAME_STATS=1 BB_GAME_DIR="$game" bash run.sh > "$log" 2>&1 &
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
