# Bloodborne for macOS

Run your own copy of *Bloodborne* (PS4) on an Apple Silicon Mac.

This is a macOS port of [bbport](https://github.com/deadinside28/bloodborne_pc), the native PC
port of Bloodborne, by way of [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc)
and [Lantern Engine](https://github.com/Awakenchan/LanternEngine). It is not an emulator. The game's
own x86-64 code runs under Rosetta 2, a small runtime stands in for the PS4 system libraries, and
the graphics are translated to Vulkan (a renderer derived from shadPS4) and then to Metal by
MoltenVK.

> **No game files are included.** You need your own decrypted copy of Bloodborne
> **CUSA03173 with update 1.09**. This project is not affiliated with Sony Interactive
> Entertainment, FromSoftware, Apple, AMD or the shadPS4 project. Please report problems
> [here](https://github.com/djrobson5/bloodborne-for-macos/issues), not to the upstream projects.

**Status: experimental, playable.** The game boots, loads saves and plays, with sound, controller
and saving. It has been tested on **one machine**: an M5 Pro with 64 GB of memory, on macOS
27.0.1. There it runs at about 60–85 FPS in play at 1080p with an uncapped
frame rate. Heavy areas drop to 45–60 FPS as the chip heats up. A full play-through has not been
verified. See [Known issues](#known-issues).

## Requirements

- **An Apple Silicon Mac** (M1 or later). Only an M5 Pro has been tested; slower chips will run
  slower.
- **macOS 15 Sequoia or later.** The game needs AVX, which Rosetta 2 supports from macOS 15.
  Only macOS 27 has been tested.
- **Memory:** the game uses 11–15 GB while running. 32 GB or more is recommended; 16 GB machines
  are untested and may not have enough.
- **Disk:** about 30 GB for the game files, plus 1 GB for this port (4 GB when building from
  source).
- **Your game files:** a decrypted dump of Bloodborne CUSA03173 with update 1.09, from a console
  you own (see [step 2](#2-prepare-your-game-files)).
- A controller is recommended (DualSense, DualShock 4 or Xbox, over USB or Bluetooth).
  Keyboard and mouse work too.

## 1. Get the port

Install Rosetta 2 and the Xcode Command Line Tools first (both are needed either way; the
Command Line Tools provide Python 3). In Terminal:

```sh
softwareupdate --install-rosetta --agree-to-license
xcode-select --install
```

Then pick **A** (download) or **B** (build it yourself).

### A. Download the prebuilt version

1. Download `bloodborne-for-macos-<version>.zip` from
   [Releases](https://github.com/djrobson5/bloodborne-for-macos/releases) and unzip it, for example
   into your home folder.
2. The download is not signed by Apple, so macOS blocks it until you clear its quarantine flag.
   In Terminal:

   ```sh
   cd ~/bloodborne-for-macos-<version>
   xattr -dr com.apple.quarantine .
   ```

### B. Build from source

1. Install [Homebrew](https://brew.sh), then the build tools:

   ```sh
   brew install cmake ninja pkgconf nasm glslang
   ```

2. Get the source, with its submodules:

   ```sh
   git clone --recursive https://github.com/djrobson5/bloodborne-for-macos.git
   cd bloodborne-for-macos
   ```

3. Build the x86-64 libraries the port links against: SDL3, the Vulkan loader, FFmpeg, Boost and
   others, at pinned versions, plus the MoltenVK 1.4.2 release. They go into `deps/`. This takes
   10–30 minutes and only has to be done once.

   ```sh
   ./scripts/macos-deps.sh
   ```

4. Build the port (a few minutes; `./bloodborne.sh` also rebuilds it when the source changes):

   ```sh
   ./build.sh
   ```

## 2. Prepare your game files

The port needs a folder that looks like this, with update 1.09 already merged in:

```
Bloodborne/
├── eboot.bin
├── dvdroot_ps4/
├── sce_module/
└── sce_sys/
    └── param.sfo
```

Where to get it depends on what you have:

- **An extracted folder** (for example from a PS4 dumper) that already includes update 1.09: use it
  as is.
- **Separate game and update folders:** copy everything from the 1.09 update folder over the game
  folder, replacing files (`eboot.bin` and `sce_sys` too).
- **A `.pkg` file:** use the included extractor. It needs the .NET SDK, version 8 or later
  (`brew install --cask dotnet-sdk`). The first argument is the new game folder; then come the
  game package and, if it doesn't already include 1.09, the update package:

  ```sh
  tools/macos/extract-pkg.sh ~/Games/Bloodborne ~/Downloads/game.pkg [~/Downloads/update-1.09.pkg]
  ```

  It builds [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) with a fix for a .NET bug that
  otherwise silently corrupts most compressed files. Don't use an unpatched LibOrbisPkg build on
  .NET 6 or later. Tested with a single package that already includes 1.09; its output matched a
  known-good dump file for file. The separate game-plus-update case is untested.

The game folder is checked at every start. If it is the wrong version or region, the log says
what is wrong (for example, a missing 1.09 update).

## 3. Play

From the port's folder (the unzipped download or your clone):

```sh
./bloodborne.sh ~/Games/Bloodborne
```

The folder is remembered, so after the first time `./bloodborne.sh` alone is enough.

- **Choose Play Offline** on the title screen. Online play comes from upstream and has not been
  tested on a Mac.
- **The first start takes longer.** The game executable is prepared once, and shaders are
  compiled the first time each one is needed. Expect stutter in the first minutes and in each new
  area; it goes away as the shader cache (`user/cache`) fills.
- **Fullscreen:** F11 (on a Mac keyboard, fn+F11), or `fullscreen=1` in `bbport.ini`. Not yet
  tested on a Mac.
- **Quit:** close the window. The game saves on its own, as on the PS4.
- **Saves** are in `user/savedata` inside the port's folder. Back that folder up before updating
  the port.
- **Logs:** each start writes `logs/<date-time>.log`. Attach it when you report a problem.

### Controls

- **Controller:** works as on the PS4. If several are connected, pick one with
  `./bloodborne.sh BB_GAMEPAD="DualSense"` (any part of its name).
- **Keyboard and mouse** (the PC controls from bbhost, not yet tested on a Mac): WASD moves, Left Option walks, Space
  dodges and sprints, Q locks on, E interacts, R uses a Blood Vial, F uses an item and Escape opens
  the menu. The left mouse button attacks (with Shift, a strong attack), the right button fires and
  the middle button transforms the weapon. G opens gestures and T personal effects. Key bindings
  are in the game's System → Key Bindings menu.
- **Port settings menu:** L3+R3 on a controller, or Insert on a keyboard that has one. It holds the
  frame-rate display, the upscaler and the graphics options.

### Settings

Settings live in `bbport.ini` in the port's folder, and the port settings menu writes most of
them. To change the frame rate for one start, pass it on the command line:

```sh
./bloodborne.sh BB_FPS=60      # uncap (default), 30, 60 or 90
```

Only the defaults (1080p output, uncapped frame rate) have been tested on a Mac. Other output
resolutions, presets and upscalers come from bbport and may or may not work with MoltenVK.

### Uninstalling

Delete the port's folder, after backing up `user/savedata` if you want to keep your saves. The
Vulkan pipeline cache is also stored in `~/Library/Application Support/shadPS4/cache/CUSA03173*`,
the same place shadPS4 uses; delete those entries too unless you also use shadPS4.

## Known issues

- **A crash while an area loads** can still happen, rarely. The main cause was fixed (0 crashes in
  73 test boots, against 5 in 55 before), but long play sessions have not been tested yet.
- **A large brown, flat shape following the camera** was seen once after a lock-on. Reloading the
  area (resting at a lamp or reloading the save) clears it.
- **Slower in long sessions:** the frame rate drops by up to 20% over the first minutes as the
  chip heats up and its clock falls. Plugging in and using High Power mode (on Macs that have it)
  helps.
- **Untested so far:** the Laurence cinematic at a high frame rate, the DLC areas and the end of
  the game.

### Troubleshooting

The macOS-specific optimizations can each be turned off for one start, to check whether one of them
causes a problem:

| Turns off | Setting |
|---|---|
| Lossless render-target compression | `BB_IMAGE_COMPRESS=0` |
| Early command submission | `BB_EARLY_SUBMIT=0` |
| Waiting for occlusion query results | `BB_OCCLUSION_WAIT_US=0` |
| Untracked Metal heaps | `BB_UNTRACKED_HEAPS=0` |
| Clear folding | `BB_FOLD_CLEARS=0` |
| Object motion vector fixes | `BB_MOTION_PLAIN_STORE=0 BB_MOTION_SLOT_FIX=0` |
| Draw windows for buffers past 4 GiB | `BB_ARENA_WINDOWS=0` |

For example: `./bloodborne.sh BB_IMAGE_COMPRESS=0`. If a setting fixes the problem, please say so
in the issue.

## How the macOS port works

The upstream port runs the game's code directly on an x86-64 Linux or Windows PC. On a Mac:

- **Everything in the game process is x86-64 and runs under Rosetta 2:** the game, the runtime,
  the renderer and the libraries. Rosetta handles the game's AVX, BMI1, MOVBE and LZCNT
  instructions but only reports them with `ROSETTA_ADVERTISE_AVX=1`, which `run.sh` sets.
- **Memory layout:** the guest's memory starts at 0x74_0000_0000, out of the way of the range macOS
  reserves for the GPU. The runtime's own heap is kept below 1 TiB, where the game's code expects
  host pointers. The game's thread-local reads are redirected to a pthread key.
- **Window and graphics:** the window loop runs on the main thread, as Cocoa requires, with a Metal
  surface. MoltenVK has no sparse buffers, so the GPU's view of the game's memory uses dense 4 GiB
  arenas.
- **Metal-specific work:**
  - buffers are freed only after every earlier submission has finished; MoltenVK loses the device
    otherwise ([MoltenVK#2876](https://github.com/KhronosGroup/MoltenVK/issues/2876));
  - allocations go into untracked Metal heaps;
  - shaders are compiled without `NoContraction`, which MoltenVK turned into slow unoptimized
    functions;
  - occlusion queries wait for MoltenVK's late availability, so the game's culling works;
  - colour targets are created compressible;
  - draws past 4 GiB in a buffer go through windows, because Metal reads only 32 bits of those
    offsets;
  - submission is split up so the GPU starts earlier.

The commits starting with "macOS:" describe each change.

## Credits

This port stands on the work of many people.

**The code this repository is built from**

| | |
|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 | The port itself: the offline eboot conversion, the runtime, the renderer's extensions (two-stage draw pipeline, motion vectors, upscalers) and most of what makes Bloodborne run outside a PS4 |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | The video core and shader recompiler the renderer is derived from |
| [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) | The fork this port starts from (v0.4.0 preview): PC controls, widescreen, online play, the Windows port and many fixes. Also devilismyfriend's contributions there |
| [Lantern Engine](https://github.com/Awakenchan/LanternEngine) by Awakenchan | Its engine changes are the base of the macOS work (`bbhost-changes.patch`) |
| [droogie/bbhost](https://github.com/droogie/bbhost) | Engine hooks, keyboard and mouse controls, camera, widescreen and menu work, ported in by DarkIzuku (see [licenses/BBHOST-PORTS.md](licenses/BBHOST-PORTS.md)) |
| Credited by Lantern Engine | VRAM cleanup from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR); crash and controller fixes from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc), [Supermedo](https://github.com/Supermedo/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc); controller mappings from [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB); the trophy list from [pyL1nx/bloodborne_pc](https://github.com/pyL1nx/bloodborne_pc) |
| Game patches | Kyo, Lance McDonald, auser1337, illusion, emoose and other community members (`patches/Bloodborne.xml`) |

**Other Mac efforts we learned from.** No code was copied from these, but their findings saved a
lot of time:

| | |
|---|---|
| [Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port) | A parallel Mac port with a careful measurement log. Lossless texture compression, Metal's 32-bit draw offsets and several debugging pitfalls came from there |
| [bloodborne-native/bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) ([runtime](https://github.com/bloodborne-native/bloodborne-runtime), [recomp](https://github.com/bloodborne-native/bloodborne-recomp)) | A Rosetta-free Mac port with its own x86-to-arm64 translator. Its notes on MoltenVK drawing wrongly past 4 GiB led to the draw windows here, and its stability notes list the bbport fixes still to port |
| [KytyPS5 #721](https://github.com/KytyPS5/KytyPS5/pull/721), [AnyPS5-macos #55](https://github.com/giljakoki/AnyPS5-macos/pull/55), [LichtFeld-Studio #2664](https://github.com/MrNeRF/LichtFeld-Studio/pull/2664) | Fixes for the same MoltenVK device loss in other projects, which helped pin down the cause here |

**Libraries and tools:**
- graphics: [MoltenVK](https://github.com/KhronosGroup/MoltenVK) and the Vulkan
  [loader](https://github.com/KhronosGroup/Vulkan-Loader) and
  [headers](https://github.com/KhronosGroup/Vulkan-Headers) (Khronos);
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator);
  [sirit](https://github.com/shadps4-emu/sirit);
  [glslang](https://github.com/KhronosGroup/glslang);
  [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn, with the AMD FidelityFX SDK;
- system and media: [SDL3](https://github.com/libsdl-org/SDL); [FFmpeg](https://ffmpeg.org);
  [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9);
- interface: [Dear ImGui](https://github.com/ocornut/imgui); DejaVu fonts;
- general purpose: [Boost](https://www.boost.org); [fmt](https://github.com/fmtlib/fmt);
  [xxHash](https://github.com/Cyan4973/xxHash); [Zydis](https://github.com/zyantific/zydis);
  [xbyak](https://github.com/herumi/xbyak); [robin-map](https://github.com/Tessil/robin-map);
  [magic_enum](https://github.com/Neargye/magic_enum); [miniz](https://github.com/richgel999/miniz);
  [half](https://half.sourceforge.net/); [cpp-httplib](https://github.com/yhirose/cpp-httplib);
- game files: [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) by maxton, for extracting
  packages.

## License

bbport and this port are licensed under the GNU GPL, version 2 or later ([LICENSE](LICENSE)). The
parts adapted from bbhost are GPL-3.0-or-later, so builds that contain them, including this
repository's, are distributed under **GPL-3.0-or-later** (see [licenses/](licenses)). Third-party
components keep their own licenses. The prebuilt download includes them in
`licenses/third-party`.

The upstream bbport README, with background on how the engine works, is kept in
[docs/bbport-README.md](docs/bbport-README.md).
