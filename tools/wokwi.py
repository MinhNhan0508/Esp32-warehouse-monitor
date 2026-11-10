"""Package the final Warehouse Monitor sources for Wokwi and Arduino CLI."""

import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "targets/esp32-wokwi/warehouse-local"
BUILD = ROOT / ".build/wokwi"


def sources():
    return [
        *sorted(TARGET.glob("*.ino")),
        *sorted(TARGET.glob("*.h")),
        TARGET / "libraries.txt",
        TARGET / "diagram.json",
    ]


def bundle(output=BUILD):
    output = Path(output)
    destination = output / "warehouse-local"
    destination.mkdir(parents=True, exist_ok=True)
    hashes = {}
    with zipfile.ZipFile(output / "warehouse-local.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for source in sources():
            data = source.read_bytes()
            (destination / source.name).write_bytes(data)
            hashes[source.relative_to(ROOT).as_posix()] = hashlib.sha256(data).hexdigest()
            archive.writestr("sketch.ino" if source.suffix == ".ino" else source.name, data)
    manifest = {"warehouse-local": hashes}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"warehouse-local: {destination}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["bundle"])
    parser.add_argument("--output-dir", type=Path, default=BUILD)
    args = parser.parse_args()
    bundle(args.output_dir)


if __name__ == "__main__":
    main()
