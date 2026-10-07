#!/usr/bin/env python3
"""Zips the source tree (no build output) as dist/sm2mario-src-<version>.zip.

Uses the packager's ROM guard: the archive is refused if anything in it looks
like an N64 ROM.
"""
import json
import pathlib
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from package import ROOT, check_zip_has_no_rom, rom_reason  # noqa: E402

SKIP_DIRS = {".git", "dist", "__pycache__"}


def wanted(rel: pathlib.PurePosixPath) -> bool:
    parts = rel.parts
    if any(p in SKIP_DIRS or p.startswith("build") for p in parts):
        return False
    return True


def main() -> int:
    info = json.loads((ROOT / "package" / "info.json").read_text(encoding="utf-8"))
    out = ROOT / "dist" / f"sm2mario-src-{info['version']}.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    files = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = pathlib.PurePosixPath(path.relative_to(ROOT).as_posix())
        if not wanted(rel):
            continue
        data = path.read_bytes()
        why = rom_reason(str(rel), data)
        if why:
            raise SystemExit(f"REFUSING: {rel} {why}. Never ship the ROM.")
        if data[:2] == b"MZ":
            # Windows binaries don't belong in the source zip (in particular
            # not Microsoft's d3dcompiler_47.dll or the game's executable).
            raise SystemExit(f"REFUSING: {rel} is a Windows executable; the source zip holds source only.")
        files.append((path, rel))
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for path, rel in files:
            z.write(path, f"sm2mario/{rel}")
    check_zip_has_no_rom(out)
    print(f"wrote {out} ({len(files)} files, {out.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
