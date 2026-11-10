# Setup

## Requirements

- Python 3.12 or newer. The configuration tools and dashboard server use only the
  standard library; pip dependencies are for development tests.
- Arduino CLI 1.5.0, Arduino-ESP32 3.3.12 and the exact libraries below.
- A browser and a Wokwi ESP32 project.
- Your own Firebase Realtime Database and Email/Password Authentication, with
  separate device-writer and dashboard-viewer accounts.

Clone the source and run commands from its root:

```sh
git clone https://github.com/MinhNhan0508/Esp32-warehouse-monitor.git
cd Esp32-warehouse-monitor
```

No compilers, prebuilt firmware or saved login are included in source.

## Install the firmware dependencies

For a new Arduino CLI installation:

```sh
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install --no-deps "DHT sensor library for ESPx@1.19.0" "Adafruit SSD1306@2.5.17" "Adafruit GFX Library@1.13.0" "Adafruit BusIO@1.17.4" "ArduinoJson@7.3.2"
```

Keep an existing CLI configuration. A separate configuration can be selected with
`--config-file`; the firmware build tool also accepts `--config-file PATH` and
`--arduino-cli PATH`. The build tool checks the installed core and library versions.

## Configure Firebase

1. Create a Realtime Database and enable Email/Password sign-in in Firebase
   Authentication. Create two different users: a device writer and a viewer.
2. Copy the users' UIDs, your Web API key and the exact HTTPS database origin.
3. Generate the public firmware/web identifiers and matching Rules:

```sh
python tools/warehouse_cloud.py configure --api-key "YOUR_WEB_API_KEY" --database-url "https://YOUR_DATABASE.firebasedatabase.app" --device-uid "YOUR_DEVICE_UID" --viewer-uid "YOUR_VIEWER_UID"
```

The command updates these local files:

- `targets/esp32-wokwi/warehouse-local/warehouse_cloud_config.h`
- `web/warehouse/config.json`
- `firebase/database.rules.json`

Publish the generated Rules in your own Firebase Console. The tool does not
create accounts or deploy Rules. The initial Rules deny all access; the generated
Rules allow the device UID to write validated records and the viewer UID to read.
There is no default personal database in the published source. Keep passwords,
tokens and service-account keys out of source and recordings.

## Build and load Wokwi

```sh
python tools/warehouse_firmware.py
python tools/wokwi.py bundle
```

| Output | Use |
| --- | --- |
| `.build/wokwi-upload/warehouse-monitor.merged.bin` | Full-flash Wokwi firmware image |
| `.build/wokwi-upload/warehouse-monitor.elf` | Matching compiled ELF |
| `.build/wokwi-upload/manifest.json` | BUILD ID, source fingerprint and artifact hashes |
| `.build/wokwi/warehouse-local.zip` | Final target sources with web-editor `sketch.ino` |

Use `targets/esp32-wokwi/warehouse-local/diagram.json` in Wokwi and retain the
ESP32 attribute `"cpuFrequency": "max"`. Save the circuit. Use **F1 → Upload Firmware
and Start Simulation** to select the merged binary, then match the Serial
`BUILD id=` to the manifest. The ordinary web Run/build can use a different framework.

Configuration changes produce a new BUILD ID. The demo's build identity is not
an expected identity for a freshly configured checkout.

## Start the device and dashboard

On Windows, double-click `OPEN_DASHBOARD.cmd`, or run on Windows/Linux/macOS:

```sh
python tools/warehouse_cloud.py open
```

The server listens at `http://127.0.0.1:8080/`. Keep its process running.
`serve` starts without opening a browser; `--port 8081` selects another port.
The `open` command reuses a matching dashboard already running on that port.

In Wokwi, wait for `NEED_LOGIN`, then enter the following lines separately:

```text
log off
cloud login YOUR_DEVICE_EMAIL YOUR_DEVICE_PASSWORD
cloud status
```

Wait for completed uploads (`published` increasing, `last_http=200` or `204`).
Sign in to the dashboard with the **viewer** account and wait for LIVE. Credentials
are entered again after a device restart; neither role's password belongs in a file.

Set DHT22 near 25°C / 65% RH and close the door. Expect NORMAL and SD READY once
the sensor has recovered and the workers are ready. Refer to
[wiring](wiring.md), [testing](testing.md) and [troubleshooting](troubleshooting.md).

For VS Code tasks, create `.venv` as described in testing, open
`Warehouse-Monitor.code-workspace`, and choose a Warehouse task from **Run Task**.
