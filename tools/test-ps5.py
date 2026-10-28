#!/usr/bin/env python3
"""Run portable PS5 backend checks on the host with ASan and UBSan.
These exercise mapping/decoder/queue logic, not console API implementation.
"""
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/ps5/tests"
out.mkdir(parents=True, exist_ok=True)
clang = shutil.which("clang")
if not clang:
    raise SystemExit("Host clang is required")
common = [clang, "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
          "-DRESOT_PS5=1", "-I" + str(root / "port/ps5/compat"),
          "-I" + str(root / "port/ps5/src"), "-I" + str(root / "port/third_party/xboxrecomp/src")]
common += ["-I" + str(root / "port/third_party/xboxrecomp/src/kernel")]
for name, extra in [("pad", ["pad_map.c"]), ("audio", ["audio_ps5.c"]), ("mmio", [])]:
    sources = [root / f"port/ps5/tests/{name}_test.c"] + [root / f"port/ps5/src/{x}" for x in extra]
    binary = out / name
    subprocess.run([*common, *map(str, sources), "-pthread", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], check=True, text=True, capture_output=True, timeout=20)
    (out / f"{name}.log").write_text(result.stdout + result.stderr)
    print(result.stdout.strip())

# The native platform header is installed by the pinned PS5 dependency build.
sdk_headers = root / "build/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk/target/include"
# Use only the platform API declarations, not FreeBSD's libc headers on the host.
platform_headers = root / "build/ps5/tests/include"
platform_headers.mkdir(exist_ok=True)
source = sdk_headers / "ps5platform"
if not source.is_dir():
    raise SystemExit("Build the pinned PS5 platform dependency before the memory contract test")
link = platform_headers / "ps5platform"
if link.is_symlink() and link.resolve() != source.resolve():
    link.unlink()  # left by a checkout that has since moved
if not link.exists():
    link.symlink_to(source, target_is_directory=True)
binary = out / "memory"
subprocess.run([*common, "-DRESOT_PS5_DIRECT_MEMORY=1", "-DRESOT_RUNTIME_RETAIL=1", "-I" + str(platform_headers),
                str(root / "port/ps5/tests/memory_test.c"), str(root / "port/ps5/src/memory_ps5.c"),
                str(root / "port/third_party/xboxrecomp/src/kernel/guest_vmem.c"),
                "-pthread", "-o", str(binary)], check=True)
result = subprocess.run([str(binary)], check=True, text=True, capture_output=True, timeout=20)
(out / "memory.log").write_text(result.stdout + result.stderr)
print(result.stdout.strip())
