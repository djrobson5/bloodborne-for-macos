#!/usr/bin/env bash
# Extracts a Bloodborne .pkg you own into a game folder this port runs, with LibOrbisPkg
# (maxton/LibOrbisPkg, LGPL-3.0) built from source with liborbispkg-net8.patch:
#   - it targets .NET 8 (the project's netcoreapp3.0 is no longer installable) and runs on any
#     later .NET as well;
#   - PFSCReader reads each compressed block fully. On .NET 6+ DeflateStream.Read returns partial
#     reads, so unpatched, most compressed files come out silently corrupt.
#
# Usage: tools/macos/extract-pkg.sh GAME_DIR GAME.pkg [UPDATE.pkg]
#   GAME_DIR    the new game folder (must not exist yet)
#   GAME.pkg    the game; a package that already has update 1.09 merged in is enough
#   UPDATE.pkg  the 1.09 update, extracted over the game when the game package is older
# Needs the .NET SDK 8 or later (brew install --cask dotnet-sdk) and about 30 GB free next to
# GAME_DIR.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
die() { echo "extract-pkg: $*" >&2; exit 1; }
(( $# >= 2 )) || die 'usage: tools/macos/extract-pkg.sh GAME_DIR GAME.pkg [UPDATE.pkg]'
out=$1; shift
[[ ! -e $out ]] || die "$out already exists"
for pkg in "$@"; do [[ -f $pkg ]] || die "no such file: $pkg"; done
command -v dotnet >/dev/null || die 'the .NET SDK is missing: brew install --cask dotnet-sdk'
dotnet --list-sdks | awk -F. '$1 >= 8 { found = 1 } END { exit !found }' ||
    die 'the .NET SDK 8 or later is missing (dotnet --list-sdks): brew install --cask dotnet-sdk'

tool=${BB_LIBORBISPKG_DIR:-$here/.liborbispkg}
commit=643477263b2644e0803e0f58b8726ea4e3f3b7d4
dll=$tool/PkgTool.Core/bin/Release/net8.0/PkgTool.Core.dll
if [[ ! -f $dll ]]; then
    echo '== building LibOrbisPkg PkgTool'
    if [[ ! -d $tool/.git ]]; then git clone --quiet https://github.com/maxton/LibOrbisPkg.git "$tool"; fi
    git -C "$tool" checkout --quiet --force "$commit"
    git -C "$tool" apply --whitespace=nowarn "$here/liborbispkg-net8.patch"
    dotnet build "$tool/PkgTool.Core/PkgTool.Core.csproj" -c Release --nologo > "$tool/build.log" 2>&1 ||
        { tail -20 "$tool/build.log" >&2; die "the build failed (log: $tool/build.log)"; }
fi
# A newer .NET runtime runs it when the 8.0 one is not installed.
pkgtool() { DOTNET_ROLL_FORWARD=${DOTNET_ROLL_FORWARD:-Major} dotnet "$dll" "$@"; }

mkdir -p "$(dirname -- "$out")"
work=$out.extracting
rm -rf "$work"
mkdir -p "$work"
# PKG entries that are not in the package's file system but belong in sce_sys.
entries=(PARAM_SFO:param.sfo ICON0_PNG:icon0.png PIC1_PNG:pic1.png PLAYGO_CHUNK_DAT:playgo-chunk.dat
         SHAREPARAM_JSON:shareparam.json TROPHY__TROPHY00_TRP:trophy/trophy00.trp)
for pkg in "$@"; do
    echo "== extracting $(basename -- "$pkg") (this takes a while)"
    pkgtool pkg_extract "$pkg" "$work"
    mkdir -p "$work/uroot/sce_sys/trophy"
    listing=$(pkgtool pkg_listentries "$pkg")
    for pair in "${entries[@]}"; do
        name=${pair%%:*} file=${pair#*:}
        index=$(awk -v n="$name" '$NF == n { print $4 }' <<< "$listing")
        if [[ -n $index ]]; then pkgtool pkg_extractentry "$pkg" "$index" "$work/uroot/sce_sys/$file" > /dev/null; fi
    done
done
[[ -f $work/uroot/eboot.bin ]] || die "no eboot.bin in the package (output kept in $work)"
mv "$work/uroot" "$out"
rm -rf "$work"

# The same check the game runs at start: Bloodborne CUSA03173 with the 1.09 executable.
if ! python3 - "$repo/scripts" "$out" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import game_check
problem = game_check.problem(sys.argv[2])
if problem:
    print('extract-pkg: ' + game_check.explain(*problem), file=sys.stderr)
    sys.exit(1)
EOF
then
    die "extracted to $out, but this is not a game version the port runs"
fi
echo "Game folder ready: $out"
echo "Start it with: ./bloodborne.sh \"$out\""
