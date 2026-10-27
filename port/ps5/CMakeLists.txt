cmake_minimum_required(VERSION 3.24)
project(reSOT_ps5 C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_C_COMPILER_TARGET STREQUAL "x86_64-sie-ps5")
    message(FATAL_ERROR "Use cmake/ps5.cmake: these archives must contain PS5 objects")
endif()
get_filename_component(RESOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/dependencies.json")
file(READ "${CMAKE_CURRENT_LIST_DIR}/dependencies.json" resot_dependency_lock)
string(JSON resot_upstream_release GET "${resot_dependency_lock}" upstream_release)
string(JSON resot_upstream_commit GET "${resot_dependency_lock}" upstream)
set(RT "${RESOT_ROOT}/port/third_party/xboxrecomp/src")
set(RESOT_TARGET xbox-retail CACHE STRING "Xbox revision: xbox-demo or xbox-retail")
set_property(CACHE RESOT_TARGET PROPERTY STRINGS xbox-demo xbox-retail)
if(NOT RESOT_TARGET MATCHES "^(xbox-demo|xbox-retail)$")
    message(FATAL_ERROR "Unsupported RESOT_TARGET")
endif()
if(RESOT_TARGET STREQUAL "xbox-demo")
    set(runtime_define RESOT_RUNTIME_DEMO=1)
else()
    set(runtime_define RESOT_RUNTIME_RETAIL=1)
endif()
set(dir "${RESOT_ROOT}/source/${RESOT_TARGET}")
file(GLOB_RECURSE gen CONFIGURE_DEPENDS "${dir}/gen/*.c")
if(NOT gen)
    message(FATAL_ERROR "No lifted sources for ${RESOT_TARGET}; restore upstream sources")
endif()
set(runtime_sources
    platform/win32_compat.c
    kernel/kernel_path.c kernel/kernel_pool.c kernel/kernel_rtl.c
    kernel/kernel_memory.c kernel/kernel_file.c kernel/kernel_thread.c
    kernel/kernel_sync.c kernel/kernel_hal.c kernel/kernel_xbox.c
    kernel/kernel_ob.c kernel/kernel_io.c kernel/kernel_crypto.c
    kernel/kernel_thunks.c kernel/kernel_bridge.c kernel/xbox_memory_layout.c
    kernel/guest_vmem.c kernel/icall_feedback.c kernel/recomp_trace.c
    kernel/nv2a_pb_scan.c kernel/nv2a_pb_exec.c kernel/nv2a_vsh_interp.c
    kernel/nv2a_combiner.c audio/dsound_device.c audio/wma_decoder.c
    apu/apu_core.c apu/apu_vp.c apu/apu_dsp.c apu/apu_mmio_hook.c
    usb/ohci.c usb/usb_gamepad.c
    video/video_player.c video/video_pump.c video/fb_present.c)
list(TRANSFORM runtime_sources PREPEND "${RT}/")
# Archives are a cross-compilation gate, not a runnable game. Graphics/audio
# host services are linked only by a complete host; do not substitute stubs.
add_library(resot_runtime STATIC ${runtime_sources})
add_library(resot_diagnostics OBJECT src/diagnostics.c src/native_start.c src/video_ps5.c src/font_ps5.c)
add_library(resot_host STATIC src/input_ps5.c src/pad_map.c src/audio_ps5.c src/mmio_ps5.c)
add_library(resot_game STATIC "${RESOT_ROOT}/port/game/recomp_manual.c"
    "${RESOT_ROOT}/port/game/resot_menu_pointer.c"
    "${RESOT_ROOT}/port/windows/src/scene.c" "${dir}/target_hooks.c" ${gen})
foreach(lib resot_runtime resot_game resot_host resot_diagnostics)
    target_compile_definitions(${lib} PRIVATE RESOT_PS5=1 ${runtime_define})
    target_include_directories(${lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat"
        "${dir}" "${dir}/gen" "${RT}" "${RT}/kernel" "${RT}/platform"
        "${RT}/apu" "${RT}/nv2a" "${RT}/usb" "${RT}/input" "${RT}/audio"
        "${RT}/video" "${RT}/d3d" "${RESOT_ROOT}/port/third_party/xboxrecomp/include/xbox")
    target_compile_options(${lib} PRIVATE -fwrapv -fsigned-char -ffp-contract=off
        -fno-strict-aliasing -ffunction-sections -fdata-sections -Wno-format
        -Wno-unused-value -Wno-parentheses-equality -Wno-pointer-sign)
endforeach()

option(RESOT_PS5_GPU "Compile the native SDL3/Vulkan renderer" OFF)
if(RESOT_PS5_GPU)
    set(RESOT_SDL_SOURCE "${RESOT_ROOT}/build/deps/sdl3" CACHE PATH "Patched SDL3 source")
    set(RESOT_SDL_BUILD "${RESOT_ROOT}/build/ps5/sdl3" CACHE PATH "PS5 SDL3 build")
    set(RESOT_NATIVE_SHADERS "${RESOT_ROOT}/build/ps5/native-shaders" CACHE PATH "DXC host shaders")
    foreach(required "${RESOT_SDL_BUILD}/libSDL3.a" "${RESOT_NATIVE_SHADERS}/nv2a_native_shaders.h")
        if(NOT EXISTS "${required}")
            message(FATAL_ERROR "Missing renderer dependency: ${required}")
        endif()
    endforeach()
    add_library(SDL3::SDL3-static STATIC IMPORTED GLOBAL)
    set_target_properties(SDL3::SDL3-static PROPERTIES IMPORTED_LOCATION "${RESOT_SDL_BUILD}/libSDL3.a"
        INTERFACE_INCLUDE_DIRECTORIES "${RESOT_SDL_SOURCE}/include")
    add_subdirectory("${RESOT_ROOT}/port/render" shader-compiler)
    target_compile_definitions(resot_runtime PRIVATE RESOT_PS5_GPU=1)
    target_include_directories(resot_runtime PRIVATE "${RESOT_NATIVE_SHADERS}" "${RESOT_ROOT}/port/render")
    target_link_libraries(resot_runtime PUBLIC SDL3::SDL3-static resot_shader_compiler)
    target_compile_definitions(resot_runtime PRIVATE RESOT_PS5_DIRECT_MEMORY=1)
    target_compile_definitions(resot_host PRIVATE RESOT_PS5_DIRECT_MEMORY=1
        resot_android_data_dir=resot_ps5_data_dir)
    target_include_directories(resot_runtime PRIVATE "${PS5_PAYLOAD_SDK}/target/include")
    target_sources(resot_host PRIVATE src/memory_ps5.c src/font_ps5.c src/video_ps5.c src/buttons_ps5.c
        "${RESOT_ROOT}/port/android/src/options_android.c"
        "${RESOT_ROOT}/port/android/src/host_files.c"
        "${RESOT_ROOT}/port/android/src/sha256.c")
    add_library(resot_start OBJECT src/main_ps5.c src/native_start.c)
    target_compile_definitions(resot_start PRIVATE RESOT_PS5=1 ${runtime_define})
    target_compile_definitions(resot_start PRIVATE
        RESOT_UPSTREAM_RELEASE="${resot_upstream_release}"
        RESOT_UPSTREAM_COMMIT="${resot_upstream_commit}")
    target_include_directories(resot_start PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat"
        "${dir}" "${RT}" "${RT}/kernel" "${RT}/platform" "${RT}/apu"
        "${RESOT_SDL_SOURCE}/include")
endif()
