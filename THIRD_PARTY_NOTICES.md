# Third-party notices

## External development/runtime software

Arduino CLI, native C compilers, Python, HTTPX, pytest,
Playwright and Ruff retain their respective upstream licenses.
Their executables and Python environments are not included in tracked project
source. Pinned Python dependency versions are in `requirements.lock`.

The dashboard uses system fonts and native browser SVG; it loads no external
font, icon pack, chart library, analytics or CDN asset.

## DHT sensor library for ESPx

External Arduino dependency: **1.19.0**, installed by Arduino/Wokwi rather than
vendored in this repository. Upstream: https://github.com/beegee-tokyo/DHTesp
Library headers credit Mark Ruys and Bernd Giesecke; the supplied LICENSE is
GNU GPL version 3. Preserve upstream notices/license with any redistribution of
that dependency. The final target's `libraries.txt` pins its registry version.
Arduino registry archive SHA-256:
`24a5bcab04be57484ee3408cb9746dc572b070d3493ea4fd4e9b208b29503472`.

Arduino-ESP32 is an external framework (**3.3.12**); its
FreeRTOS and other components retain their own upstream notices. No ESP32
framework or compiler binaries are committed here.

## Adafruit OLED dependencies (warehouse local station)

External Arduino libraries, installed separately rather than vendored here:

| Library | Version | Upstream / license |
| --- | --- | --- |
| Adafruit SSD1306 | 2.5.17 | https://github.com/adafruit/Adafruit_SSD1306 — BSD 3-clause |
| Adafruit GFX Library | 1.13.0 | https://github.com/adafruit/Adafruit-GFX-Library — BSD 2-clause |
| Adafruit BusIO | 1.17.4 | https://github.com/adafruit/Adafruit_BusIO — MIT |

Copyright attribution belongs to Adafruit Industries and the upstream contributors;
retain their supplied license files/notices when distributing those dependencies.
The local station's `libraries.txt` pins the Arduino libraries, including DHTesp.

## ArduinoJson

External dependency **7.3.2**, MIT license, copyright Benoit Blanchon and
contributors. Upstream: https://github.com/bblanchon/ArduinoJson
Arduino registry archive SHA-256:
`75b39f5b0fa856b6ff36a60fc56a0ef0cab0365bed84efd3eaf95e6fc89d8776`.
Installed separately by Arduino/Wokwi; retain its upstream license on redistribution.

## Google Trust Services root certificates

`warehouse_ca.h` includes public GTS Root R1 and R4 certificates published at
https://pki.goog/repository/ for TLS server authentication, with source URLs and
SHA-256 in `third_party/warehouse-ca-manifest.json`. These are public trust
anchors, not client private keys. Certificate subjects/issuers are retained in
the original PEM contents. Review trust roots when upstream certificate chains change.

## Firebase development emulator tools

Firebase CLI 15.34.2 (https://github.com/firebase/firebase-tools, MIT) and the
Google Firebase emulators are external development tools, not vendored runtime
binaries. Java/Node are required only for these development checks, not for the
browser/ESP32 demo. Preserve each tool's own license when redistributing it.
