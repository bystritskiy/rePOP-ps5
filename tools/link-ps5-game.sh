#!/usr/bin/env bash
# reSOT PS5, MIT. Run in resot-ps5-builder with this checkout mounted at /work.
# The pinned RADV link recipe and native runtime are GPL-3.0-or-later.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-xbox-retail}
case "$target" in xbox-retail|xbox-demo) ;; *) exit 2;; esac
vulkan="$root/build/deps/PS5_Vulkan"
# The pinned RADV recipe writes shared temporary maps inside its source tree.
# Serialize links across both targets, including separate Docker invocations.
mkdir -p "$root/build/ps5"
exec 9>"$root/build/ps5/link.lock"
flock 9
sdk="$vulkan/.deps/native/ps5-payload-sdk"
archive="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
build="$root/build/ps5/game-$target"
work="$root/build/ps5/linked-$target"
mkdir -p "$work/obj" "$work/stubs"
export LLVM_CONFIG=/usr/bin/llvm-config-18 PS5_CLANG=/usr/bin/clang-18
export PATH=/usr/lib/llvm-18/bin:$PATH
resource=$(clang-18 --print-resource-dir)
mkdir -p "$resource/lib/linux"
cp "$root/build/deps/compiler-rt/libclang_rt.builtins-x86_64.a" "$resource/lib/linux/"
cc="$sdk/bin/prospero-clang"
cxx="$sdk/bin/prospero-clang++"
"$cxx" -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$vulkan/tooling/native/app_cpp_runtime.cpp" -o "$work/obj/cpp-runtime.o"
for pair in 'libSceAgc agc_canary_link_stub.c' 'libSceAgcDriver agc_driver_canary_link_stub.c'; do
    read -r library source <<<"$pair"
    "$cc" -std=c11 -O2 -fPIC -c "$vulkan/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/stubs/$library.so" "$work/obj/$library.o"
done
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive"
# The recipe groups its C++ archives; flatten that group into our app group.
inputs=()
for item in "${radv_link_inputs[@]}"; do
    case "$item" in --start-group|--end-group) continue;; esac
    inputs+=("$item")
done
# Use one visibility map: multiple anonymous maps can re-export operators.
flags=();skip=0
for item in "${radv_link_flags[@]}"; do
    if ((skip)); then skip=0;continue;fi
    if [[ $item == --version-script ]]; then skip=1;continue;fi
    flags+=("$item")
done
{
    printf '{local:\n_Zn*; _Zd*;\n'
    for item in "${flags[@]}"; do
        if [[ $item == --defsym=* ]]; then
            name=${item#--defsym=};printf '%s;\n' "${name%%=*}"
        fi
    done
    printf '*;};\n'
} > "$work/exports.map"
shaders=()
while IFS= read -r -d '' file; do shaders+=("$file"); done < <(find "$build/shader-compiler" -name '*.a' -print0)
# Stubs describe dynamic imports. Their function bodies never enter eboot.bin.
stubs=()
for file in "$sdk"/target/lib/*.so; do
    case "$file" in *libScePosixForWebKit.so|*libSceGLSlimVSH.so|*libc_stub_weak.so) continue;; esac
    stubs+=("$file")
done
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" "${flags[@]}" \
    --version-script "$work/exports.map" --exclude-libs=ALL --no-dynamic-linker --gc-sections \
    -e _start -o "$work/llvm-pie.elf" \
    "$build/CMakeFiles/resot_start.dir/src/main_ps5.c.o" \
    "$build/CMakeFiles/resot_start.dir/src/native_start.c.o" "$work/obj/cpp-runtime.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    --start-group "$build/libresot_game.a" "$build/libresot_runtime.a" "$build/libresot_host.a" \
    "$root/build/ps5/sdl3/libSDL3.a" "${shaders[@]}" "${inputs[@]}" \
    "$sdk/target/lib/libc.a" "$sdk/target/lib/libpthread.a" --end-group \
    --as-needed "${stubs[@]}"
llvm-readelf-18 --dyn-syms --wide "$work/llvm-pie.elf" > "$work/imports.txt"
llvm-readelf-18 --program-headers --wide "$work/llvm-pie.elf" > "$work/segments.txt"
