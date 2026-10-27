#!/usr/bin/env python3
"""Check a user's extracted Xbox game against the supported lifted-code hashes.
Read-only; does not extract, copy, modify or package the input.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("game_directory", type=Path)
    args = p.parse_args()
    folder = args.game_directory.resolve()
    if not folder.is_dir():
        p.error("Supply an extracted Xbox disc directory containing default.xbe, not a PS2 ISO")
    files = {}
    for path in folder.iterdir():
        key = path.name.casefold()
        if key in files:
            p.error(f"Ambiguous case-insensitive disc filename: {path.name}")
        files[key] = path
    xbe = files.get("default.xbe")
    if xbe is None or not xbe.is_file():
        p.error("default.xbe missing: this port needs the original Xbox version")
    actual = hashlib.sha256(xbe.read_bytes()).hexdigest().upper()
    target = None
    for candidate in ["xbox-demo", "xbox-retail"]:
        text = (ROOT / "source" / candidate / "target.h").read_text()
        expected = re.search(r'#define\s+RESOT_XBE_SHA256\s+"([a-fA-F0-9]+)"', text)[1].upper()
        if actual == expected:
            target = candidate
    if not target:
        p.error(f"Unsupported default.xbe revision (SHA-256 {actual}); do not reuse another revision's addresses")
    required = ["prince.bf"] + (["shaders.bin"] if target == "xbox-retail" else [])
    missing = [name for name in required if name not in files or not files[name].is_file()]
    if missing:
        p.error("Incomplete game directory: missing " + ", ".join(missing))
    print(json.dumps({"target": target, "directory": str(folder), "xbe_sha256": actual,
                      "disc_files_present": True, "validation_scope": "game_files_only"}, indent=2))


if __name__ == "__main__":
    main()
