# Testing and results

## Automated checks

Use Python 3.12+ and a native GCC or Clang compiler. Set `CC` to its executable
if it is not on PATH. Portable C tests exercise the exact Warehouse headers used
by the ESP32; they do not emulate peripherals or measure ESP32 task timing.

```sh
python -m venv .venv
```

Activate with `.venv\Scripts\activate` on Windows or `source .venv/bin/activate`
on Linux/macOS, then run:

```sh
python -m pip install -r requirements.lock
python -m playwright install chromium
python -m ruff check tools tests
python -m ruff format --check tools tests
python -m pytest tests -q --ignore=tests/test_firebase_rules.py --junitxml=.build/tests.xml
```

On Ubuntu install GCC with `sudo apt-get install gcc`, and browser system
dependencies with `python -m playwright install --with-deps chromium`.

| Suite | Scope |
| --- | --- |
| Portable C | Alarm hysteresis, sensor validity/recovery, stale boundary, door/ACK, rollover, bounded CSV, cloud retry/UTC/identifier policies |
| Configuration/package | Matching firmware/Rules/web identifiers, malformed configuration rejection, source hashes and diagram connections |
| Browser | Mocked Firebase login/logout, role rejection, alarms, storage state, freshness, SSE/polling, token refresh, charts and CSV |
| Firebase Emulators | Device/viewer access, malformed payloads, timestamps, deletion and bounded history slots |
| ESP32 compiler | Final target with cloud disabled and enabled using dummy identifiers |

Browser tests explicitly mock Firebase traffic. Generated mock screenshots stay
under `.build` and are not live cloud evidence.

## Firebase Rules tests

Requires Node.js 24, Java 21 and Firebase CLI 15.34.2. Clear an inherited `DEBUG`
environment variable, then run:

```sh
npm install --global firebase-tools@15.34.2
firebase emulators:exec --project demo-warehouse --only auth,database "python -m pytest tests/test_firebase_rules.py -q --junitxml=.build/firebase-rules-tests.xml"
```

Auth and Database bind to localhost ports 9099/9000. Tests create fake local users
and generated Rules only in the reserved demo project; never substitute a real
project ID. These 18 tests skip in ordinary pytest without emulator variables.

## Firmware checks

Install the pinned Arduino dependencies in [setup](setup.md), then run:

```sh
python tools/wokwi.py bundle
python tools/warehouse_firmware.py
python tools/warehouse_compile_profile.py
arduino-cli compile --fqbn esp32:esp32:esp32 --jobs 2 --warnings all --build-path .build/esp32/cloud-profile .build/cloud-profile/warehouse-local
```

The first build uses the checkout configuration, initially cloud disabled. The
separate compile profile enables cloud using dummy public identifiers without
changing checkout configuration or contacting Firebase. The build manifest records
BUILD ID and SHA-256. Static compiler sizes do not measure peak runtime memory.

## Recorded Wokwi/Firebase behavior

The [demo](assets/warehouse/warehouse-demo-web.mp4) shows temperature/RH alarms,
door monitoring, ACK, injected sensor fault/recovery, SD event inspection and CSV
export. Screenshots come from different moments and boots. This runtime evidence
is separate from mocked browser tests and local emulators. Rebuilding for new
Firebase identifiers requires separate runtime verification.

| Manual scenario | Expected observation |
| --- | --- |
| 25°C / 65% RH, closed door | Valid readings, NORMAL, SD READY and LIVE after uploads |
| Temperature 32 → 31 → 30°C | Alarm, hysteresis hold, clear |
| Humidity 75 → 73 → 70% | Alarm, hold, clear |
| Door open for 10 s, then ACK | Door alarm; mute without clearing the cause |
| `fault on`, then `fault off` | Invalid values hidden; recovery after two good reads |
| `sd fail on`, then `sd fail off` | Storage fault/skipped records, then appends resume |
| `cloud off`, then `cloud on` | Local sensing continues; cloud data becomes stale, then fresh |
| `cloud dnsfail on/off` | Explicit software upload-DNS failure and recovery; not every real Internet outage |

Inspect `status`, `sd status`, `sd read`, `sd events` and `cloud status`; reset
software injections after testing. Keep login commands out of captured logs.

Local validation of this release passed **56 tests** covering portable C,
configuration, wiring, packaging and the mocked dashboard. Static checks also passed.
All **18 Auth/Database Emulator tests** also passed in a separate local run.

Both local Windows ESP32 builds passed with Arduino-ESP32 3.3.12 and the pinned libraries:

| Compile profile | Program bytes | Static/global bytes |
| --- | --- | --- |
| Cloud disabled | 462,897 | 36,776 |
| Cloud enabled, dummy identifiers | 1,152,711 | 58,764 |

These compiler figures are not measurements of peak heap or task stack use.

Current automated results are in [GitHub Actions](https://github.com/MinhNhan0508/Esp32-warehouse-monitor/actions/workflows/ci.yml).
CI checks software and compilation; it does not execute the ESP32 circuit in
Wokwi or validate a live Firebase deployment. No physical calibration, production
uptime, power-loss durability or full fresh-machine GUI installation is claimed.
