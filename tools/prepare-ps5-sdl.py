#!/usr/bin/env python3
"""Patch the pinned, ignored SDL3 source for native PS5 display WSI."""
from pathlib import Path
import shutil,subprocess,sys
root=Path(__file__).resolve().parent.parent
source=Path(sys.argv[1]).resolve()
revision=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
if revision!='829a65d769d935c4852f8159e964312c0957260a':raise SystemExit('SDL3 must be release-3.4.18')
subprocess.run([sys.executable,str(root/'tools/patch-sdl-gpu-query.py'),str(source)],check=True)
shutil.copyfile(root/'port/ps5/sdl/SDL_ps5vulkan.c',source/'src/video/offscreen/SDL_offscreenvulkan.c')
p=source/'src/video/offscreen/SDL_offscreenvideo.c';s=p.read_text().replace('mode.w = 1024;','mode.w = 1920; /* RE-SoT: PS5 VideoOut default. */').replace('mode.h = 768;','mode.h = 1080; /* RE-SoT: PS5 VideoOut default. */');p.write_text(s)
# PS5 RADV reports conformance 0.0.0.0 on purpose (no CTS release has passed
# on the console); SDL then refuses the only device: "SDL_HINT_GPU_DRIVER vulkan
# unsupported!" on the console. Accept RADV itself, nothing else non-conformant.
p=source/'src/gpu/vulkan/SDL_gpu_vulkan.c';s=p.read_text()
old='            isConformant = (physicalDeviceDriverProperties.conformanceVersion.major >= 1);\n'
mark='RE-SoT (PS5): RADV without a CTS release'
if mark not in s:
    assert s.count(old)==1
    s=s.replace(old,old+'            /* '+mark+' is the console driver. */\n            if (physicalDeviceDriverProperties.driverID == VK_DRIVER_ID_MESA_RADV) isConformant = true;\n')
    p.write_text(s)
print('Prepared SDL3 with PS5 VideoOut WSI and reSOT GPU queries')
