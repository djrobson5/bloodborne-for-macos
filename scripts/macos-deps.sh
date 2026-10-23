#!/usr/bin/env bash
# Builds the x86-64 libraries the macOS port links against into deps/ (or the directory given as
# the first argument) and writes deps/env.sh, which build.sh and run.sh read.
#
# Everything the game process loads is x86-64 and runs under Rosetta 2, so these libraries are
# built for x86-64 even on an arm64 Mac. The versions are the ones the port was developed and
# tested with; MoltenVK is the official 1.4.2 release binary (checked by SHA-256).
#
# Needs: Xcode Command Line Tools, Rosetta 2, and from Homebrew: cmake ninja pkgconf nasm glslang.
# Takes 20-40 minutes and ~3 GB of disk; reruns skip what is already installed.
# Usage: scripts/macos-deps.sh [DEPS_DIR]
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
root=${1:-$repo/deps}
mkdir -p "$root"
root=$(cd -- "$root" && pwd)
prefix=$root/x86_64
src=$root/src
build=$root/build
host=$root/host/bin
jobs=$(sysctl -n hw.ncpu)

die() { echo "macos-deps: $*" >&2; exit 1; }
[[ $(uname -s) == Darwin ]] || die 'this script is for macOS'
[[ $(uname -m) == arm64 ]] || echo 'macos-deps: not an Apple Silicon Mac; continuing, but only Apple Silicon is tested' >&2
arch -x86_64 /usr/bin/true 2>/dev/null || die 'Rosetta 2 is not installed: softwareupdate --install-rosetta --agree-to-license'
xcrun --show-sdk-path >/dev/null 2>&1 || die 'Xcode Command Line Tools are missing: xcode-select --install'
missing=()
for tool in cmake ninja pkg-config nasm glslang curl git; do command -v "$tool" >/dev/null || missing+=("$tool"); done
if (( ${#missing[@]} )); then
    die "missing ${missing[*]}; install them with: brew install cmake ninja pkgconf nasm glslang"
fi

# Host tools (arm64) the GPU library's build runs. They are linked into a directory of their own
# because the x86-64 configure below hides /opt/homebrew (its arm64 libraries must not be found).
mkdir -p "$host" "$src" "$build" "$prefix"
for tool in glslang glslangValidator nasm; do
    if command -v "$tool" >/dev/null; then ln -sf "$(command -v "$tool")" "$host/$tool"; fi
done
export PATH="$host:$PATH"

# name, repository, tag, commit (checked after the clone)
sources=(
    "SDL https://github.com/libsdl-org/SDL.git release-3.4.18 829a65d769d935c4852f8159e964312c0957260a"
    "Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git v1.4.363 6802bb4733b63ed5efd3adb308a6c885ef180ea1"
    "Vulkan-Loader https://github.com/KhronosGroup/Vulkan-Loader.git vulkan-sdk-1.4.363.0 fa218055ff4948b159f31d43f800c808a13d51df"
    "VulkanMemoryAllocator https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.4.0 3aa921224c154a0d2c43912bc88e1c42ce1f7607"
    "fmt https://github.com/fmtlib/fmt.git 11.2.0 40626af88bd7df9a5fb80be7b25ac85b122d6c21"
    "magic_enum https://github.com/Neargye/magic_enum.git v0.9.8 1384769c66bd16ec9bb1353f45fe8ec8ccc12dbd"
    "miniz https://github.com/richgel999/miniz.git 3.1.1 d10b03cc73475af673df40f06e5cefd1d5f940d9"
    "robin-map https://github.com/Tessil/robin-map.git v1.4.1 bd14e6830a1474fed9d2d03f5c3b0683d818d540"
    "xbyak https://github.com/herumi/xbyak.git v7.43 cd6bad3cad2e07a3e277c23d2b1f065320024717"
    "xxHash https://github.com/Cyan4973/xxHash.git v0.8.3 e626a72bc2321cd320e953a0ccf1584cad60f363"
    "zydis https://github.com/zyantific/zydis.git v4.1.1 a2278f1d254e492f6a6b39f6cb5d1f5d515659dc"
    "FFmpeg https://github.com/FFmpeg/FFmpeg.git n7.1.5 3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587"
)
boost_url=https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
boost_sha=aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
moltenvk_url=https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
moltenvk_sha=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e

fetch() {
    local name=$1 url=$2 tag=$3 commit=$4
    if [[ ! -d $src/$name/.git ]]; then
        echo "== fetch $name $tag"
        git clone --quiet --depth 1 --branch "$tag" --recurse-submodules --shallow-submodules "$url" "$src/$name"
    fi
    local head
    head=$(git -C "$src/$name" rev-parse HEAD)
    [[ $head == "$commit" ]] || die "$name: $tag is $head, expected $commit"
}

download() {
    local url=$1 sha=$2 file=$3
    if [[ ! -f $file ]] || ! shasum -a 256 -c <<< "$sha  $file" >/dev/null 2>&1; then
        echo "== download $(basename "$file")"
        curl -fsSL --retry 3 -o "$file.part" "$url"
        shasum -a 256 -c <<< "$sha  $file.part" >/dev/null || die "$(basename "$file"): SHA-256 mismatch"
        mv "$file.part" "$file"
    fi
}

# x86-64 CMake build and install; a stamp file skips it on reruns.
cm() {
    local name=$1 dir=$2; shift 2
    if [[ -f $build/$name.done ]]; then return; fi
    echo "== build $name"
    cmake -S "$dir" -B "$build/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -DCMAKE_FIND_FRAMEWORK=LAST \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" > "$build/$name.log" 2>&1 \
        || { tail -30 "$build/$name.log" >&2; die "$name: configure failed (log: $build/$name.log)"; }
    { cmake --build "$build/$name" -j "$jobs" && cmake --install "$build/$name"; } >> "$build/$name.log" 2>&1 \
        || { tail -30 "$build/$name.log" >&2; die "$name: build failed (log: $build/$name.log)"; }
    touch "$build/$name.done"
}

for entry in "${sources[@]}"; do
    # shellcheck disable=SC2086
    fetch $entry
done

cm vulkan-headers "$src/Vulkan-Headers" -DVULKAN_HEADERS_ENABLE_TESTS=OFF
cm vulkan-loader "$src/Vulkan-Loader" -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF
cm vma "$src/VulkanMemoryAllocator"
cm sdl "$src/SDL" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cm fmt "$src/fmt" -DFMT_TEST=OFF -DFMT_DOC=OFF
cm magic_enum "$src/magic_enum" -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
cm miniz "$src/miniz" -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_FUZZERS=OFF -DBUILD_SHARED_LIBS=OFF
cm robin-map "$src/robin-map"
cm xbyak "$src/xbyak"
cm xxhash "$src/xxHash/cmake_unofficial" -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
cm zycore "$src/zydis/dependencies/zycore" -DZYCORE_BUILD_SHARED_LIB=OFF -DZYCORE_BUILD_TESTS=OFF -DZYCORE_BUILD_EXAMPLES=OFF
cm zydis "$src/zydis" -DZYDIS_BUILD_SHARED_LIB=OFF -DZYDIS_BUILD_TESTS=OFF -DZYDIS_BUILD_TOOLS=OFF \
    -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF -DZYDIS_BUILD_MAN=OFF

download "$boost_url" "$boost_sha" "$src/boost-1.90.0-cmake.tar.xz"
if [[ ! -d $src/boost-1.90.0 ]]; then tar -xf "$src/boost-1.90.0-cmake.tar.xz" -C "$src"; fi
# Boost.Context would pick the host's (arm64) assembly without the target spelled out.
cm boost "$src/boost-1.90.0" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DBOOST_CONTEXT_ARCHITECTURE=x86_64 -DBOOST_CONTEXT_ABI=sysv \
    "-DBOOST_INCLUDE_LIBRARIES=container;icl;intrusive;pool;asio;preprocessor;container_hash"

# FFmpeg (video playback): a static x86-64 build with only the decoders and demuxers the game uses.
if [[ ! -f $build/ffmpeg.done ]]; then
    echo "== build FFmpeg"
    mkdir -p "$build/ffmpeg"
    (
        cd "$build/ffmpeg"
        "$src/FFmpeg/configure" --prefix="$prefix" --enable-cross-compile --arch=x86_64 --target-os=darwin \
            --cc='clang -arch x86_64' --cxx='clang++ -arch x86_64' --extra-ldflags='-arch x86_64' \
            --extra-cflags='-mmacosx-version-min=13.0' --extra-ldflags='-mmacosx-version-min=13.0' \
            --disable-programs --disable-doc --disable-network --disable-autodetect --enable-static \
            --disable-shared --enable-pic --disable-avdevice --disable-avfilter --disable-postproc \
            --disable-everything --enable-avformat --enable-avcodec --enable-avutil --enable-swscale \
            --enable-swresample \
            --enable-decoder='h264,hevc,aac,aac_latm,mp3,mp3float,mp2,mp2float,atrac9,vorbis,opus,vp9,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_u8,pcm_s8,pcm_alaw,pcm_mulaw' \
            --enable-demuxer='mov,matroska,wav,mp3,aac,ogg,h264,hevc' \
            --enable-parser='h264,hevc,aac,aac_latm,mpegaudio,vorbis,opus,vp9' \
            --enable-bsf='h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata,vp9_superframe_split' \
            --enable-protocol=file --enable-zlib
        make -j "$jobs"
        make install
    ) > "$build/ffmpeg.log" 2>&1 || { tail -30 "$build/ffmpeg.log" >&2; die "FFmpeg: build failed (log: $build/ffmpeg.log)"; }
    touch "$build/ffmpeg.done"
fi

# MoltenVK: the official release (a universal binary; the x86-64 half is the one loaded).
download "$moltenvk_url" "$moltenvk_sha" "$src/MoltenVK-macos.tar"
if [[ ! -f $prefix/lib/libMoltenVK.dylib ]]; then
    echo "== install MoltenVK 1.4.2"
    rm -rf "$build/moltenvk" && mkdir -p "$build/moltenvk"
    tar -xf "$src/MoltenVK-macos.tar" -C "$build/moltenvk"
    cp "$build/moltenvk/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$prefix/lib/"
    mkdir -p "$prefix/share/licenses/MoltenVK"
    cp "$build/moltenvk/MoltenVK/LICENSE" "$prefix/share/licenses/MoltenVK/"
fi
# A relative library path: the Vulkan loader resolves it against the manifest's own directory.
mkdir -p "$prefix/share/vulkan/icd.d"
cat > "$prefix/share/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF

# The environment build.sh and run.sh source. Relocatable: paths are found from its own location.
cat > "$root/env.sh" <<'EOF'
# Source this file (bash): . deps/env.sh   (written by scripts/macos-deps.sh)
_DEPS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export DEPS_PREFIX="$_DEPS_ROOT/x86_64"
# Only this prefix: Homebrew's arm64 libraries must not be found by pkg-config.
export PKG_CONFIG_LIBDIR="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH=
export CMAKE_PREFIX_PATH="$DEPS_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PATH="$_DEPS_ROOT/host/bin:$PATH"
export VK_ICD_FILENAMES="$DEPS_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json"
export DYLD_LIBRARY_PATH="$DEPS_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
EOF

# Every library the port links must be x86-64.
bad=()
for lib in "$prefix"/lib/*.a "$prefix"/lib/*.dylib; do
    [[ -L $lib ]] && continue
    lipo -archs "$lib" 2>/dev/null | grep -qw x86_64 || bad+=("$(basename "$lib")")
done
(( ${#bad[@]} == 0 )) || die "not x86-64: ${bad[*]}"
echo "Dependencies ready in $root (environment: $root/env.sh)"
