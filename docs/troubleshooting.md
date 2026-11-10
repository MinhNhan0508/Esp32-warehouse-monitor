# Troubleshooting

| Symptom | Checks |
| --- | --- |
| Dashboard URL unavailable | Run `python tools/warehouse_cloud.py open` and leave the server running. |
| Port occupied | Reuse the matching dashboard or choose `open --port 8081`. An unrelated server is not reused. |
| `CLOUD_CONFIG_REQUIRED` | Configure your identifiers, rebuild, upload the new merged image, and match its BUILD ID. |
| `NEED_LOGIN` | Enter the device account in Serial; repeat after a device restart. |
| Authentication error | Check your API key, Email/Password provider, credentials and role. The dashboard requires the viewer UID. |
| Upload HTTP 401/403 | Check device UID and generated Rules in your Firebase project; inspect `cloud status`. |
| STALE / old values | Check simulation is running and `published` increases. A retained record is not a new reading. |
| CLOCK MISMATCH | Check computer UTC and firmware time synchronization; pausing/throttling simulation can interrupt delivery. |
| TLS/network failure | Retain `cpuFrequency: max`, keep Wokwi active, and check network/clock probes. Do not disable certificate verification. |
| Wrong Serial banner | Upload the merged binary; ordinary web Run may build a different framework. |
| Sensor unavailable | Clear injection with `fault off`; recovery requires two valid readings. Check diagram wiring. |
| SD unavailable | Clear `sd fail on` with `sd fail off`, inspect `sd status`, and use `sd retry` after fixing the cause. |
| Build rejects dependencies | Install pinned versions from [setup](setup.md); select the matching CLI/config path. |
| SDK headers appear missing on Windows | Use a short physical Arduino data/tools directory and select it with `--config-file`; deeply nested toolchains can exceed compiler path limits. |
| C compiler missing | Install native GCC/Clang or set `CC` to its executable. |
| Browser missing | Run `python -m playwright install chromium`; on Linux include `--with-deps`. |
| Rules tests skipped | Run through Firebase `emulators:exec` with Node 24 and Java 21 available. |

LIVE means fresh device status, even if the sensor is invalid or an alarm is
active. Read sensor, alarm and storage status together. Wokwi SD behavior does
not establish physical power-loss durability.
