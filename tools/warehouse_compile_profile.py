"""Prepare an enabled-cloud Arduino compile profile with public dummy identifiers."""

import shutil
from pathlib import Path

from warehouse_cloud import ROOT, configure, identifiers


def main():
    profile = ROOT / ".build/cloud-profile"
    target = profile / "warehouse-local"
    shutil.copytree(ROOT / "targets/esp32-wokwi/warehouse-local", target, dirs_exist_ok=True)
    config = identifiers(
        "DEMO_PUBLIC_API_KEY_123456789",
        "https://demo-warehouse-default-rtdb.asia-southeast1.firebasedatabase.app",
        "DEMO_DEVICE_UID",
        "DEMO_VIEWER_UID",
    )
    header, _, _ = configure(config, profile)
    shutil.copyfile(header, target / Path(header).name)
    print(target)


if __name__ == "__main__":
    main()
