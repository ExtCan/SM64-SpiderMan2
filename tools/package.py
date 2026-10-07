#!/usr/bin/env python3
"""Packages Mario Mode as an Overstrike .script mod.

Layout inside the zip (Overstrike extracts everything except info.json into
<game>/scripts/ and loads the first .dll whose path does not start with
"resources" - so sm2mario.dll must come before anything else):

    info.json
    sm2mario.dll
    resources/sm2mario/sm64.dll
    resources/sm2mario/sm2mario.ini
    resources/sm2mario/bindings.ini
    resources/sm2mario/PUT_YOUR_ROM_HERE.txt
"""
import argparse
import hashlib
import json
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Super Mario 64 is copyrighted: the mod reads the player's own ROM at runtime
# and must never ship one. Every file that goes into a package is checked.
ROM_EXTENSIONS = {".z64", ".n64", ".v64", ".u64", ".j64", ".rom", ".ndd"}
N64_MAGICS = (b"\x80\x37\x12\x40", b"\x37\x80\x40\x12", b"\x40\x12\x37\x80")
SM64_SHA1 = {"9bef1128717f958171a4afac3ed78ee2bb4e86ce"}  # US .z64 (other regions: caught by the header checks)


def rom_reason(name: str, data: bytes) -> str:
    """Why `data` (packaged as `name`) looks like an N64 ROM, or '' if it doesn't."""
    if pathlib.PurePosixPath(name.lower()).suffix in ROM_EXTENSIONS:
        return "has an N64 ROM file extension"
    if data[:4] in N64_MAGICS:
        return "starts with an N64 ROM header"
    if hashlib.sha1(data).hexdigest() in SM64_SHA1:
        return "is a Super Mario 64 ROM (SHA-1 match)"
    for magic in N64_MAGICS:
        # A ROM embedded in another file (archive, resource blob).
        idx = data.find(magic)
        while idx != -1:
            title = data[idx + 0x20 : idx + 0x34]
            if b"SUPER MARIO 64" in title or b"USPERM RAIO 64" in title or b"PUSER AMIR6O 4" in title:
                return "contains an embedded Super Mario 64 ROM"
            idx = data.find(magic, idx + 1)
    return ""


def assert_no_rom(entries) -> None:
    for name, src in entries:
        data = pathlib.Path(src).read_bytes()
        why = rom_reason(name, data)
        if why:
            raise SystemExit(f"REFUSING TO PACKAGE {name} ({src}): it {why}. Never ship the ROM.")


def check_zip_has_no_rom(path: pathlib.Path) -> None:
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            why = rom_reason(name, z.read(name))
            if why:
                path.unlink()
                raise SystemExit(f"REFUSING: {path.name} would contain {name}, which {why}. Deleted the package.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dll", required=True, help="built sm2mario.dll")
    ap.add_argument("--sm64", required=True, help="built libsm64 sm64.dll")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="output folder")
    args = ap.parse_args()

    dll = pathlib.Path(args.dll)
    sm64 = pathlib.Path(args.sm64)
    for f in (dll, sm64):
        if not f.is_file():
            print(f"missing: {f}", file=sys.stderr)
            return 1

    info_path = ROOT / "package" / "info.json"
    info = json.loads(info_path.read_text(encoding="utf-8"))
    for key in ("name", "type", "version", "dependencies", "game"):
        if key not in info:
            print(f"info.json lacks '{key}' (Overstrike needs it)", file=sys.stderr)
            return 1
    if info["game"] != "MSM2":
        print("info.json 'game' must be MSM2", file=sys.stderr)
        return 1

    res = ROOT / "package" / "resources" / "sm2mario"
    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"sm2mario-{info['version']}.script"

    entries = [
        ("info.json", info_path),
        ("sm2mario.dll", dll),
        ("resources/sm2mario/sm64.dll", sm64),
        ("resources/sm2mario/sm2mario.ini", res / "sm2mario.ini"),
        ("resources/sm2mario/bindings.ini", res / "bindings.ini"),
        ("resources/sm2mario/PUT_YOUR_ROM_HERE.txt", res / "PUT_YOUR_ROM_HERE.txt"),
    ]
    assert_no_rom(entries)
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name, src in entries:
            z.write(src, name)
    check_zip_has_no_rom(target)

    # Sanity: first non-resources DLL must be ours.
    with zipfile.ZipFile(target) as z:
        dlls = [n for n in z.namelist() if n.lower().endswith(".dll") and not n.lower().startswith("resources")]
        assert dlls and dlls[0] == "sm2mario.dll", dlls
    print(f"wrote {target} ({target.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
