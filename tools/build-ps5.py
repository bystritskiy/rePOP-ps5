#!/usr/bin/env python3
"""Cross-compile PS5 archives and optionally a no-game native diagnostic app."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BASE = json.loads((ROOT / "port/ps5/dependencies.json").read_text())["upstream"]
TITLE = "PPSA99022"


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run(args, log=None):
    command = [str(x) for x in args]
    print("+", subprocess.list2cmdline(command), flush=True)
    if log:
        with log.open("w") as output:
            subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True)
    else:
        return subprocess.check_output(command, cwd=ROOT, text=True, stderr=subprocess.STDOUT)


def source_fingerprint():
    names = run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"]).split("\0")
    h = hashlib.sha256()
    count = 0
    for name in sorted(set(names)):
        path = ROOT / name
        if not path.is_file() or (path.suffix not in {".c", ".h", ".cpp", ".hpp", ".inc", ".cmake", ".ld", ".py", ".sh", ".json"}
                                  and path.name not in {"CMakeLists.txt", "Dockerfile"}):
            continue
        h.update(name.encode() + b"\0" + digest(path).encode() + b"\n")
        count += 1
    return {"files": count, "sha256": h.hexdigest()}


def inventory(root):
    return {p.relative_to(root).as_posix(): {"size": p.stat().st_size, "sha256": digest(p)}
            for p in sorted(root.rglob("*")) if p.is_file() and p.name.lower() != ".ds_store"}


def icon(path):
    # Original geometric icon, generated without external artwork or fonts.
    import struct
    import zlib
    side = 512
    scanlines = bytearray()
    for y in range(side):
        scanlines.append(0)
        for x in range(side):
            border = 85 <= x < 427 and (90 <= y < 110 or 402 <= y < 422)
            hourglass = 120 <= y < 392 and abs(x - 256) <= abs(y - 256) // 2 + 4
            scanlines.extend((232, 189, 101, 255) if border or hourglass else (18, 25, 32, 255))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def diagnostics(args, sdk, llvm, build):
    if not args.native_tool or not args.native_tool.is_file():
        raise SystemExit("--diagnostics requires --native-tool /path/to/ps5-native-tool")
    out = ROOT / "build/ps5/diagnostics"
    out.mkdir(parents=True, exist_ok=True)
    app = out / TITLE
    if app.is_symlink():
        raise SystemExit("Diagnostic staging must not be a symlink")
    app.mkdir(exist_ok=True)
    if any(p.is_symlink() for p in app.rglob("*")):
        raise SystemExit("Diagnostic staging contains a symlink")
    # This dedicated staging directory contains only generated diagnostics.
    # Refuse extra files instead of accidentally packaging an earlier game.
    allowed = {"eboot.bin", "sce_sys", "diagnostics.txt"}
    if any(p.name not in allowed for p in app.iterdir()):
        raise SystemExit(f"Unexpected files in diagnostic staging: {app}")
    exports = out / "exports.map"
    exports.write_text("{local:*;};\n")
    objects = sorted((build / "CMakeFiles/resot_diagnostics.dir/src").glob("*.o"))
    if len(objects) != 3:
        raise SystemExit("Diagnostic objects missing")
    excluded = {"libScePosixForWebKit.so", "libSceGLSlimVSH.so", "libc_stub_weak.so"}
    stubs = [p for p in sorted((sdk / "target/lib").glob("*.so")) if p.name not in excluded]
    pie = out / "llvm-pie.elf"
    run([llvm / "ld.lld", "-m", "elf_x86_64", "-pie", "-z", "max-page-size=0x4000", "--hash-style=gnu",
         "-T", ROOT / "port/ps5/native.ld", "--eh-frame-hdr", "--version-script", exports,
         "-e", "_start", "--no-dynamic-linker", "--gc-sections", "--exclude-libs=ALL", "-o", pie, *objects,
         "--start-group", build / "libresot_host.a", build / "libresot_runtime.a", sdk / "target/lib/libc.a",
         sdk / "target/lib/libpthread.a", "--end-group", "--as-needed", *stubs], out / "link.log")
    dynamic = run([llvm / "llvm-readelf", "--dyn-syms", "--wide", pie])
    imports = re.findall(r"\bUND[ \t]+(\S+)", dynamic)
    defined = [line for line in dynamic.splitlines() if re.search(r"\bGLOBAL\b", line) and " UND " not in line]
    if defined:
        raise SystemExit("Native converter cannot publish application exports: " + str(defined))
    native = args.native_tool.resolve()
    run([native, "link", "--in", pie, "--out", out / "eboot.elf", "--stub-dir", sdk / "target/lib",
         "--module-sdk", "0x02000009", "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"], out / "convert.log")
    run([native, "self", "--sign", "--in", out / "eboot.elf", "--out", app / "eboot.bin", "--magic", "0x1D3D154F"], out / "sign.log")
    inspection = run([native, "self", "--inspect", "--file", app / "eboot.bin"])
    (out / "fself-inspect.txt").write_text(inspection)
    if "integrity: valid" not in inspection:
        raise SystemExit("Diagnostic FSELF integrity check did not pass")
    meta = app / "sce_sys"
    meta.mkdir(exist_ok=True)
    param = {"titleId": TITLE, "contentId": f"UP9000-{TITLE}_00-RESOTDIAG00000001", "conceptId": TITLE[4:],
             "contentVersion": "01.000.000", "masterVersion": "01.00", "applicationCategoryType": 0,
             "applicationDrmType": "free", "attribute": 0, "attribute2": 0, "attribute3": 524352,
             "ageLevel": {"default": 0}, "contentBadgeType": 1, "downloadDataSize": 256,
             "requiredSystemSoftwareVersion": "0x0000000000000000", "sdkVersion": "0x0000000000000000",
             "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": "reSOT PS5 Diagnostics"}}}
    (meta / "param.json").write_text(json.dumps(param, indent=2) + "\n")
    icon(meta / "icon0.png")
    (app / "diagnostics.txt").write_text("reSOT PS5 diagnostics. No game data. No linked game renderer.\n"
        "Memory aliases and events run at startup. Cross tests PCM output.\n"
        "Log: /data/reSOT/PPSA99022/diagnostics.log or /download0/resot-ps5-diagnostics.log\n")
    if set(inventory(app)) != {"eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png", "diagnostics.txt"}:
        raise SystemExit("Unexpected diagnostic package contents")
    receipt = {"title_id": TITLE, "fself_integrity": "valid", "import_count": len(imports), "imports": imports,
               "native_tool_sha256": digest(native), "console_tested": False, "contains_game_data": False,
               "game_renderer_linked": False, "files": inventory(app)}
    (out / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if args.pack_python:
        dest = ROOT / "bin/ps5/reSOT-PS5-Diagnostics-PPSA99022.ffpfsc"
        dest.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="resot-pack-", dir=dest.parent) as temp:
            temp = Path(temp)
            partial = temp / "diagnostics.ffpfsc"
            run([args.pack_python, "-m", "mkpfs", "pack", "folder", "--verify", app, partial], out / "pack.log")
            extracted = temp / "extracted"
            run([args.pack_python, "-m", "mkpfs", "unpack", "--deep", partial, extracted], out / "unpack.log")
            if inventory(app) != inventory(extracted):
                raise SystemExit("Package roundtrip differs from the staged app")
            os.replace(partial, dest)
        receipt["package"] = {"path": str(dest.relative_to(ROOT)), "size": dest.stat().st_size,
                              "sha256": digest(dest), "roundtrip_files": len(inventory(app))}
        (dest.parent / "diagnostics-verification.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sdk", type=Path, required=True)
    p.add_argument("--llvm-bin", type=Path, required=True)
    p.add_argument("--target", choices=["all", "xbox-demo", "xbox-retail"], default="all")
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--diagnostics", action="store_true")
    p.add_argument("--native-tool", type=Path)
    p.add_argument("--pack-python", type=Path, help="Python environment with MkPFS 1.1.0")
    args = p.parse_args()
    sdk, llvm = args.sdk.resolve(), args.llvm_bin.resolve()
    if args.jobs < 1 or not (sdk / "target/lib/libc.a").is_file():
        p.error("Invalid job count or uninstalled PayloadSDK")
    for name in ["clang", "ld.lld", "llvm-ar", "llvm-readelf"]:
        if not (llvm / name).is_file():
            p.error(f"Missing {llvm / name}")
    for path in [sdk, llvm]:
        if any(c.isspace() for c in str(path)):
            p.error("The SDK/LLVM paths must not contain whitespace (CMake sysroot flags)")
    out = ROOT / "build/ps5"
    out.mkdir(parents=True, exist_ok=True)
    source = source_fingerprint()
    compiler = run([llvm / "clang", "--version"]).splitlines()[0]
    if not re.search(r"version 18\.", compiler):
        p.error("This toolchain is pinned to Clang 18")
    results = {"upstream_commit": BASE, "source": source, "compiler": compiler,
               "sdk_libc_sha256": digest(sdk / "target/lib/libc.a"), "console_tested": False,
               "game_linked": False, "game_renderer_linked": False, "targets": {}}
    targets = ["xbox-demo", "xbox-retail"] if args.target == "all" else [args.target]
    for target in targets:
        build = out / target
        run(["cmake", "--fresh", "-S", ROOT / "port/ps5", "-B", build, "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "port/ps5/cmake/ps5.cmake"),
             "-DCMAKE_C_COMPILER=" + str(llvm / "clang"), "-DCMAKE_AR=" + str(llvm / "llvm-ar"),
             "-DPS5_PAYLOAD_SDK=" + str(sdk), "-DCMAKE_BUILD_TYPE=Release", "-DRESOT_TARGET=" + target], out / f"configure-{target}.log")
        run(["cmake", "--build", build, "-j", args.jobs], out / f"compile-{target}.log")
        results["targets"][target] = {"objects": len(list((build / "CMakeFiles").rglob("*.c.o"))),
            "archives": {x.name: {"size": x.stat().st_size, "sha256": digest(x)} for x in sorted(build.glob("*.a"))}}
    if args.diagnostics:
        results["diagnostics"] = diagnostics(args, sdk, llvm, out / targets[-1])
    if source_fingerprint() != source:
        raise SystemExit("Sources changed while building; rerun before using this receipt")
    (out / "build-receipt.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({"receipt": str(out / "build-receipt.json"), "game_linked": False, "console_tested": False}, indent=2))


if __name__ == "__main__":
    main()
