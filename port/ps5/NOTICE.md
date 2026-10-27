# PS5 build: licences and source

The reSOT PS5 host and its SDL3 display bridge are MIT, like the upstream host.
SDL3 3.4.18 is altered by `port/ps5/sdl/SDL_ps5vulkan.c`,
`tools/prepare-ps5-sdl.py`, and the existing reSOT GPU-query patch.

The PS5 executable additionally links PS5 Platform from the pinned
PS5_PayloadSDK fork, and uses PS5_Vulkan's native link recipe, clean-room libc
module and C++ allocation runtime. These components are GPL-3.0-or-later;
the combined PS5 executable is subject to those terms. Its third-party notices
and the upstream MIT/LGPL, SDL zlib, glslang and SPIR-V notices accompany the
local build. Mesa contains several licences; its complete `licenses/` folder
and `docs/license.rst` accompany the build.

Source revisions and repositories are listed in `dependencies.json`.
Downloaded source stays in `build/deps/`. reSOT modifications stay in the
published host/runtime files and scripts; generated game C is unchanged.
The installed public PayloadSDK is not modified. No Sony SDK or system module
is distributed. `libc.prx` is the reproducibly generated clean-room runtime
from PS5_Vulkan, with its expected release SHA-256 recorded by the packager.
AGC link-stub bodies are host tooling and are not packaged or executed.

The original game assets and Xbox executables remain the user's local input;
these source licences do not grant rights to those files. The package is a
local build from that input, with no upload or publication performed.
