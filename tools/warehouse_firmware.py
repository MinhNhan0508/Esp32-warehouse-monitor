"""Build a pinned ESP32 full-flash image for Wokwi upload."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "targets/esp32-wokwi/warehouse-local"
OUTPUT = ROOT / ".build/wokwi-upload"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized(version):
    parts = version.split(".")
    return ".".join(parts + ["0"] * max(0, 3 - len(parts)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino-cli", default=shutil.which("arduino-cli") or "arduino-cli")
    parser.add_argument("--config-file", type=Path)
    args = parser.parse_args()
    cli = [args.arduino_cli]
    if args.config_file:
        cli += ["--config-file", str(args.config_file)]

    def query(*command):
        result = subprocess.run(
            [*cli, *command, "--format", "json"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=True,
        )
        return json.loads(result.stdout)

    installed = query("core", "list")["platforms"]
    version = next((p["installed_version"] for p in installed if p["id"] == "esp32:esp32"), None)
    if version != "3.3.12":
        raise SystemExit(f"Install esp32:esp32@3.3.12 first; found {version!r}")
    libraries = {
        p["library"]["name"]: p["library"]["version"]
        for p in query("lib", "list")["installed_libraries"]
    }
    pins = {}
    for line in (SOURCE / "libraries.txt").read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        name, wanted = line.rsplit("@", 1)
        if normalized(libraries.get(name, "missing")) != normalized(wanted):
            raise SystemExit(f"Install {line}; found {libraries.get(name)!r}")
        pins[name] = wanted

    paths = sorted([*SOURCE.glob("*.h"), *SOURCE.glob("*.ino"), SOURCE / "libraries.txt"])
    canonical = {p.name: sha(p) for p in paths}
    fingerprint = hashlib.sha256(json.dumps(canonical, sort_keys=True).encode()).hexdigest()
    build_id = fingerprint[:8]
    snapshot = OUTPUT / "source/warehouse-local"
    snapshot.mkdir(parents=True, exist_ok=True)
    for p in paths:
        shutil.copyfile(p, snapshot / p.name)
    sketch = snapshot / "warehouse-local.ino"
    sketch.write_text(
        f"#define WM_BUILD_ID 0x{build_id}u\n" + sketch.read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    build = ROOT / ".build/esp32/warehouse-pinned-upload"
    result = subprocess.run(
        [
            *cli,
            "compile",
            "--fqbn",
            "esp32:esp32:esp32",
            "--jobs",
            "2",
            "--warnings",
            "all",
            "--build-path",
            str(build),
            str(snapshot),
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    (OUTPUT / "compile.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)
    artifacts = {}
    for source, name in (
        (build / "warehouse-local.ino.merged.bin", "warehouse-monitor.merged.bin"),
        (build / "warehouse-local.ino.elf", "warehouse-monitor.elf"),
    ):
        destination = OUTPUT / name
        shutil.copyfile(source, destination)
        artifacts[name] = dict(bytes=destination.stat().st_size, sha256=sha(destination))
    manifest = dict(
        created_at_utc=datetime.now(timezone.utc).isoformat(),
        build_id=build_id,
        source_fingerprint=fingerprint,
        canonical_sha256=canonical,
        snapshot_sha256={p.name: sha(snapshot / p.name) for p in paths},
        core=version,
        fqbn="esp32:esp32:esp32",
        libraries=pins,
        artifacts=artifacts,
        configuration="Uses current public Firebase config. No runtime login credentials embedded.",
        scope="Compiled locally; Wokwi upload/runtime requires separate verification.",
    )
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(result.stdout.strip())
    for name in artifacts:
        print(OUTPUT / name)
    print(f"Expected boot: BUILD id={build_id} Arduino-ESP32=3.3.12")


if __name__ == "__main__":
    main()
