from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from decrypt_table import convert_data_bytes  # noqa: E402

MAGIC = b"X2DATA1"


def pack(input_dir: Path, output: Path, manifest: Path | None, selected: set[str] | None):
    entries: list[tuple[str, bytes, int, bytes]] = []
    skipped = 0
    for src in sorted(input_dir.glob("*.txt")):
        name = src.stem
        if selected is not None and name not in selected:
            skipped += 1
            continue
        encrypted = src.read_bytes()
        decrypted = convert_data_bytes(encrypted)
        entries.append((name, encrypted, len(encrypted), decrypted))

    with output.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(entries)))
        for name, encrypted, original_size, decrypted in entries:
            raw_name = name.encode("utf-8")
            f.write(struct.pack("<I", len(raw_name)))
            f.write(raw_name)
            f.write(struct.pack("<II", original_size, len(decrypted)))
            f.write(decrypted)

    index = {
        "format": "X2DATA1",
        "source": str(input_dir),
        "count": len(entries),
        "skipped": skipped,
        "entries": [
            {
                "name": name,
                "original_size": original_size,
                "size": len(decrypted),
                "sha256": hashlib.sha256(encrypted).hexdigest(),
                "decrypted_sha256": hashlib.sha256(decrypted).hexdigest(),
            }
            for name, _, original_size, decrypted in entries
        ],
    }
    manifest.write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n")
    print(f"packed {len(entries)} tables -> {output} ({output.stat().st_size} bytes); manifest={manifest}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input-dir", type=Path, default=Path("masterdata/table"))
    ap.add_argument("--output", type=Path, default=Path("data/tables.x2data"))
    ap.add_argument("--manifest", type=Path, default=Path("data/tables.json"))
    ap.add_argument("--only", help="comma-separated table names (without .txt); default all")
    args = ap.parse_args()
    selected = set(filter(None, args.only.split(","))) if args.only else None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    pack(args.input_dir, args.output, args.manifest, selected)


if __name__ == "__main__":
    main()
