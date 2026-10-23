#!/usr/bin/env bash
# Builds the prebuilt download: dist/bloodborne-for-macos-<version>.zip, with the game runner,
# its x86-64 libraries (SDL3, the Vulkan loader, MoltenVK) and their licenses. No game files.
# Usage: tools/macos/package.sh [VERSION]   (default: git describe)
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
version=${1:-$(git describe --tags --always --dirty)}
./build.sh
# shellcheck disable=SC1091
source deps/env.sh
name=bloodborne-for-macos-$version
stage=dist/$name
rm -rf "$stage" "dist/$name.zip"
mkdir -p "$stage/bin" "$stage/lib/vulkan/icd.d" "$stage/licenses/third-party" "$stage/tools/macos"

cp out/bb-probe out/bb-gpu-capabilities "$stage/bin/"
for lib in libSDL3.0.dylib libvulkan.1.dylib libMoltenVK.dylib; do cp -L "$DEPS_PREFIX/lib/$lib" "$stage/lib/"; done
for binary in "$stage"/bin/*; do
    install_name_tool -add_rpath @executable_path/../lib "$binary"
    codesign --force --sign - "$binary" 2>/dev/null
done
cat > "$stage/lib/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF

# What run.sh reads at start: the game preparation scripts and the community patches.
cp run.sh bloodborne.sh README.md LICENSE "$stage/"
mkdir -p "$stage/scripts"
cp scripts/*.py "$stage/scripts/"
cp -R patches "$stage/"
cp tools/macos/extract-pkg.sh tools/macos/liborbispkg-net8.patch "$stage/tools/macos/"
cp -R licenses/. "$stage/licenses/"

# Licenses of everything linked into or shipped with the binaries.
third=$stage/licenses/third-party
src=deps/src
copy_license() { local name=$1; shift; for f in "$@"; do [[ -f $f ]] && cp "$f" "$third/$name-$(basename "$f")"; done; }
copy_license MoltenVK "$DEPS_PREFIX/share/licenses/MoltenVK/LICENSE"
copy_license SDL "$src/SDL/LICENSE.txt"
copy_license Vulkan-Loader "$src/Vulkan-Loader/LICENSE.txt"
copy_license Vulkan-Headers "$src/Vulkan-Headers/LICENSE.md"
copy_license VulkanMemoryAllocator "$src/VulkanMemoryAllocator/LICENSE.txt"
copy_license FFmpeg "$src/FFmpeg/LICENSE.md" "$src/FFmpeg/COPYING.LGPLv2.1"
copy_license Boost "$src/boost-1.90.0/LICENSE_1_0.txt"
copy_license fmt "$src/fmt/LICENSE"
copy_license magic_enum "$src/magic_enum/LICENSE"
copy_license miniz "$src/miniz/LICENSE"
copy_license robin-map "$src/robin-map/LICENSE"
copy_license xbyak "$src/xbyak/COPYRIGHT"
copy_license xxHash "$src/xxHash/LICENSE"
copy_license Zydis "$src/zydis/LICENSE"
copy_license Zycore "$src/zydis/dependencies/zycore/LICENSE"
copy_license LibAtrac9 third_party/LibAtrac9/LICENSE
copy_license imgui gpu/third_party/imgui/LICENSE.txt
copy_license FSR-Vulkan gpu/third_party/fsr-vulkan/LICENSE gpu/third_party/fsr-vulkan/LICENSE.txt
{
    echo "bloodborne-for-macos $version"
    echo "Source: https://github.com/djrobson5/bloodborne-for-macos (commit $(git rev-parse HEAD))"
    echo 'Dependencies: built by scripts/macos-deps.sh at the versions it lists; MoltenVK 1.4.2 release.'
} > "$stage/SOURCE.txt"

(cd dist && ditto -c -k --keepParent "$name" "$name.zip")
echo "Packaged dist/$name.zip"
