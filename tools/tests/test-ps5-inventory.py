"""Finder metadata must not change the inventory checked after MkPFS unpack."""
import importlib.util
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ps5_build", root / "tools/build-ps5.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)
with tempfile.TemporaryDirectory() as temp:
    app = Path(temp)
    (app / "game/POP1").mkdir(parents=True)
    for name in ("eboot.bin", "game/default.xbe", "game/POP1/data.bf", "game/.game-data"):
        (app / name).write_bytes(name.encode())
    before = build.inventory(app)
    for name in ("game/.DS_Store", "game/POP1/.DS_Store"):
        (app / name).write_bytes(b"Finder metadata")
    assert build.inventory(app) == before, "Finder metadata changed the packaged inventory"
    assert "game/.game-data" in before, "Legitimate hidden files must not be filtered"
    (app / "game/POP1/data.bf").write_bytes(b"changed game data")
    assert build.inventory(app) != before, "Real file changes must still be detected"
print("PS5 inventory: Finder metadata ignored; game files and changes preserved PASS")
