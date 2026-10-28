# reSOT on PS5

A native PS5 host for the reSOT **Xbox retail** recompilation of Prince of
Persia: The Sands of Time. The lifted game code in `source/xbox-retail/gen`
is the same as upstream's; this port adds the PS5 host (`port/ps5`), PS5
adaptations in the shared runtime (marked `RE-SoT`), and the build,
packaging and install tools.

**Status (2026-10-28).** Based on upstream
[FulGerNet/rePOP v0.4.0](https://github.com/FulGerNet/rePOP/releases/tag/reSOTv0.4.0)
(`reSOTv0.4.0`, `828ebf32c6abea108f4d23c174191be4faa6ae2c`); PS5 content
version `01.004.000`. Retail and demo compile and link with SDL3 GPU, Vulkan
and RADV. With 01.004.000 the user saw on the console Special Load Map
loading a map, the cheats switch in play, 7601 Salle without its crash, the
Prince with his body after a map change, and a fresh install at 2160p with
the Full preset. With 01.003.006 (upstream v0.3.4) the retail build,
installed as a folder, reached the
title screen, the main menu, the profile name keyboard, the palace balcony
cutscene and the first gameplay frames, with correct lighting, fog and
textures, at about 60 fps. Button prompts show PlayStation buttons. The app
found the disc files in `/data/reSOT/xbox-retail`. **Not checked yet:**
sound, controller input in play, saves, levels past the first
frames, the demo target on a console. Details: `docs/VALIDATION.md`.

Tested environment: **FW 13.40, kstuff-lite 1.11, ShadowMountPlus
1.7-beta5fix1**. Other firmware or loaders have not been tried.

**You need your own game.** Nothing here contains the game's data or an
Xbox executable, and the app does not carry them: it reads the files of your
own disc from the console's storage.

## Game files

Extract your Xbox retail disc (for example with extract-xiso) and copy all
its files and folders to one of these folders on the console; the app uses
the first one that is complete:

1. `/app0/game` (inside the app, for a package built with `--game`)
2. `/data/reSOT/xbox-retail`
3. `/mnt/usb0/reSOT/xbox-retail` to `/mnt/usb7/...` (USB drives)
4. `/mnt/ext0/reSOT/xbox-retail`, `/mnt/ext1/...` (extended storage)

A folder is used when it holds `default.xbe`, `prince.bf`, `shaders.bin`,
`video/` and `sound/` and the XBE's SHA-256 is `RESOT_XBE_SHA256` from
`source/xbox-retail/target.h`. Another revision is refused: the lifted
addresses are only valid for the hashed file. With no usable folder the app
shows a screen that says where to copy the files and which folders it
refused and why, sends a notification, and closes on Cross.

On the Mac the same check is

```sh
python3 tools/check-ps5-game.py <extracted disc folder>
```

## Building (macOS host)

Needs Docker, Git, Python 3, CMake, Ninja and LLVM 18
(`brew install llvm@18 ninja cmake`). The first setup downloads the pinned
sources and builds RADV, which takes much longer than later game builds.
Dependencies and outputs stay in ignored `build/` and `bin/`.

```sh
python3 tools/setup-ps5-deps.py --llvm-bin /opt/homebrew/opt/llvm@18/bin

cmake -S port/ps5 -B build/ps5/game-xbox-retail -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps5/cmake/ps5.cmake" \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@18/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@18/bin/clang++ \
  -DPS5_PAYLOAD_SDK="$PWD/build/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk" \
  -DRESOT_TARGET=xbox-retail -DRESOT_PS5_GPU=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/ps5/game-xbox-retail -j8

docker run --rm -v "$PWD:/work" -w /work resot-ps5-builder:local \
  bash tools/link-ps5-game.sh xbox-retail
```

`ps5-native-tool` converts the linked PIE to the PS5 ELF layout and signs it
as a development FSELF. It is PS5_Vulkan's own tool, built from the pinned
source that `setup-ps5-deps.py` fetched:

```sh
N=build/deps/PS5_Vulkan/tooling/native
/opt/homebrew/opt/llvm@18/bin/clang++ -std=c++20 -O2 \
  $N/native_app_builder.cpp $N/self_container.cpp $N/elf_object.cpp \
  $N/sce_module_writer.cpp -lz -o build/ps5/ps5-native-tool
```

Then sign and stage the app folder:

```sh
python3 tools/package-ps5-game.py \
  --native-tool build/ps5/ps5-native-tool \
  --llvm-bin /opt/homebrew/opt/llvm@18/bin --zip
```

The folder is `build/ps5/app-package/PPSA99023/`, with no game files, and
`--zip` also writes the release archive `bin/ps5/PPSA99023-<contentVersion>.zip`
(the app folder at the top, entries sorted and dated so that a rebuild is
byte-identical). The packager checks the native imports and exports and both
FSELFs; the checks and hashes go to `bin/ps5/app-verification.json`.
`sce_sys/param.json` and `icon0.png` (the reSOT logo) come from
`port/ps5/sce_sys/`; the `contentVersion` there must match
`dependencies.json`. With `--game <folder>` the disc files are checked and
staged inside the app as hard links instead (`build/ps5/game-package/`,
for your own use only; such a package contains the game).

For a single `.ffpfsc` image, add `--pack-python` with a Python
that has [MkPFS](https://github.com/PSBrew/MkPFS) 1.1.0
(`pip install git+https://github.com/PSBrew/MkPFS.git@1d5df56ae29390900e888fa12e5f9856aa797adc`).
The image, `bin/ps5/reSOT-PS5-PPSA99023.ffpfsc`, is unpacked again and
compared file by file before it is kept.

A build that should be installed beside this app (a private variant, for
example) needs a title ID of its own: configure with
`-DRESOT_PS5_TITLE_ID=PPSAnnnnn`, which also names its data folder
`/data/reSOT/PPSAnnnnn/`, and package with `--title-id PPSAnnnnn
--content-id <contentId>`. The packager refuses a linked build whose data
folder names another ID. The default, `PPSA99023`, is the published app's.

The demo target builds the same way with `-DRESOT_TARGET=xbox-demo` in its
own build folder; it has not been packaged or run on a console.

## Installing

**Folder (recommended while developing).** ShadowMountPlus mounts
`/data/homebrew/PPSA99023/` the same way as an image (nullfs onto
`/system_ex/app/PPSA99023`), so only changed files need to be copied. With
ftpsrv running on the console (port 2121):

```sh
python3 -I tools/deploy-ps5-folder.py --addr <console address>
```

Then copy your disc files to `/data/reSOT/xbox-retail/` (any FTP client).
The script sends files under `game/` and `licenses/` only when they are missing or
changed and the rest (`eboot.bin`, `sce_*`) every time, writes each file as `NAME.part` and renames
it, and sends `sce_sys/param.json` last. Remote files that are not in the
staging folder are listed, not deleted. Do not keep an image with the same
title ID in `/data/homebrew` at the same time.

**Image.** Put `reSOT-PS5-PPSA99023.ffpfsc` in the image folder
ShadowMountPlus scans. This path has not been tried on a console yet.

The tile is **reSOT - The Sands of Time**, title ID `PPSA99023`. Options,
shader caches and saves live in `/data/reSOT/PPSA99023/`; logs in its
`logs/xbox-retail-err.txt` and `logs/xbox-retail-out.txt` (the previous run's
with `.previous`). The host also writes to the kernel log
(`nc <console address> 3232` with a klog server).

## Controls

DualSense: Cross/Circle/Square/Triangle = A/B/X/Y, L1/R1 = White/Black,
L2/R2 = the triggers, Options = Start, touchpad click = Back. One player,
reconnection and two-motor rumble. Controls can be changed in the game's
reSOT Options menu. Keyboard input is off by default.

The prompts in the game's text name buttons through font 5 (FONTDESC
`0x8d007577`, atlas texture `0x0b00450a`, 512x256 A8R8G8B8). When the
renderer decodes that atlas, recognised by the FNV-1a hash of its pixels on
the disc, `src/buttons_ps5.c` redraws in the decoded copy A/B/X/Y as
✕/○/□/△, White/Black as L1/R1, the triggers as L2/R2, Start as OPTIONS and
Back as TOUCH PAD; the GPU builds the mip chain. The game files and guest
memory are not changed. The stick, d-pad and camera glyphs and the Xbox
controller drawing on the controls screen are still the originals.

## Implementation

- Public PS5 PayloadSDK and Clang 18 (`x86_64-sie-ps5`), emulated TLS,
  16 KiB segments. No Sony SDK is used or distributed.
- Xbox RAM is one direct-memory block mapped into mirrors inside a reserved
  4 GiB range; `MAP_FIXED` is used only inside the port's own reservations.
  The retail title's upper user window (about 192 MiB) gets one direct
  backing at start; the guest tracker keeps 4 KiB Xbox reserve/commit and
  zeroing on release.
- PS5 pages are 16 KiB, Xbox pages 4 KiB. Guest protection that removes
  access applies to whole host pages only (`xbox_ProtectGuestRange`), so a
  read-only request cannot take a neighbour's data with it. Executable guest
  pages are mapped as data, without native execute.
- APU, OHCI and AC97 MMIO go through a scalar x86-64 decoder in the PS5
  signal handler (mcontext at offset 64, RIP at 0xE0, as in the public
  PayloadSDK); an unknown instruction stays a crash. eboot code is
  execute-only, so the decoder reads a copy of the code segment from
  `/app0/eboot.bin`, placed with `sceKernelGetModuleInfo`.
- The main thread takes CPUs `0x1FFF` before any other thread starts;
  stdout and stderr go through a pipe to a writer thread.
- Sound: native AudioOut, S16 stereo 48 kHz, with a bounded queue held near
  8 frames by resampling by at most 0.8%, like the Windows and Android hosts.
- SDL3 3.4.18 with a VideoOut WSI over `VK_KHR_display` and a static Vulkan
  ICD; upstream's SDL GPU-query patch is kept. Output is 3840x2160, 16:9
  (the window's size picks the VideoOut mode; VideoOut scales it to the
  screen). A new install renders at 2160p (4.5x) internal resolution with
  the Full preset and VSync on (`options_android.c` under `RESOT_PS5`); the
  INI and reSOT Options, Graphics, override them. The PS5 adds the 2160p
  internal resolution. Before 01.004.000 a new install got 1440p (3x) and
  the Standard preset: with no `GraphicsPreset` in the INI the preset follows
  the texture glow default, which the PS5 now sets.
- Movies (the NV2A video overlay) are drawn with a bicubic Catmull-Rom
  shader (`ps_movie`). A full-width movie leaves the 4:3 frame: it fills the
  16:9 picture when that crops no more than 6% of it (the 640x346 story
  movies, about 2% at each side) and otherwise fits it (640x448, 640x432,
  640x480).
- RADV from PS5_Mesa (`radv-winsys=ps5`), linked with PS5_Vulkan's recipe and
  its clean-room `libc.prx` (rebuilt and checked against the pinned digest).
- Boot checks the XBE's SHA-256 before mapping it and requires a Vulkan
  device before the game starts. `source/*/gen/` is not changed.
- Options reuse the Android host's POSIX implementation; Android's JNI,
  launcher and touch controls are not included.

Pinned revisions are in `dependencies.json`; licences in `NOTICE.md` and
`docs/LICENSING.md`.

## Tests

```sh
python3 tools/test-ps5.py
python3 tools/test-menu-pointer.py
python3 tools/tests/test-ps5-inventory.py
```

The host tests run on the Mac under ASan/UBSan: DualSense mapping;
PCM, queue and shutdown against a mock AudioOut; the scalar MMIO decoder;
the direct-memory contract with real shared mappings (mirrors, protection,
bounds, lifetime, cleanup), including the retail guest tracker's reserve,
neighbouring 4 KiB commits, decommit, recommit and shutdown. They do not
call PS5 system functions.

`tools/check-hlsl.py` needs Windows' D3DCompiler. On the Mac the unchanged
host shaders are built with the official DXC v1.9.2609 to DXIL and SPIR-V:

```sh
python3 tools/build-native-shaders.py --dxc build/deps/dxc/dxc --output build/ps5/native-shaders
python3 tools/prepare-ps5-sdl.py build/deps/sdl3
cmake --build build/ps5/sdl3 -j8
```

`tools/build-ps5.py` builds the older archive-only gate and a separate
diagnostic app without the game; it does not replace the game build above.

## Following upstream

This port follows upstream release tags. For each one:

1. `git fetch upstream tag reSOTvX.Y.Z`, review the changes since the
   `upstream` commit in `dependencies.json`, and merge the tag. Keep both
   upstream's game fixes and the PS5 adaptations when resolving conflicts.
2. In `dependencies.json` set `upstream_release`, the exact `upstream` SHA
   (`git rev-parse 'reSOTvX.Y.Z^{commit}'`) and a higher
   `ps5_content_version`. Change the pinned SDK, SDL or RADV only when needed.
3. After runtime or SDL changes rebuild the shaders and SDL as above, run
   the tests, build and link both targets, and commit before packaging: the
   packager requires the tag to match the pin and to be in HEAD's history.
4. Record in `docs/VALIDATION.md` what was run and seen, and what was not.

Title ID `PPSA99023` and `/data/reSOT/PPSA99023/` stay the same across
versions; whether saves and caches carry over is checked on the console.
