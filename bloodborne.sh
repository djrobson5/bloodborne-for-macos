#!/usr/bin/env bash
# Starts Bloodborne on macOS. The game folder is remembered after the first start.
# Usage: ./bloodborne.sh [GAME_DIR] [VAR=value ...]
#   GAME_DIR   the folder with eboot.bin (CUSA03173 with update 1.09 merged in)
#   VAR=value  extra settings for this start, e.g. BB_FPS=60 (see README)
# Each start's output is also written to logs/<date-time>.log.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$here"
saved=$here/game-dir.txt
game=${BB_GAME_DIR:-}
if [[ $# -gt 0 && $1 != *=* ]]; then game=$1; shift; fi
if [[ -z $game && -f $saved ]]; then game=$(<"$saved"); fi
if [[ -z $game ]]; then
    echo 'Usage: ./bloodborne.sh /path/to/game-folder   (the folder that contains eboot.bin)' >&2
    exit 1
fi
if [[ ! -f $game/eboot.bin ]]; then
    echo "No eboot.bin in $game. Point this at the extracted game folder (see README)." >&2
    exit 1
fi
game=$(cd -- "$game" && pwd)
printf '%s\n' "$game" > "$saved"
# A packaged download has bin/bb-probe; a source checkout builds out/bb-probe (run.sh does it).
if [[ -x bin/bb-probe ]]; then export BB_PREBUILT=1; fi
mkdir -p logs
log=logs/$(date +%Y%m%d-%H%M%S).log
echo "Game: $game"
echo "Log:  $here/$log"
env "$@" BB_GAME_DIR="$game" bash run.sh 2>&1 | tee "$log"
