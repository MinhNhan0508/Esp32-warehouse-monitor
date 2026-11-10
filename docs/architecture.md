# Architecture

The ESP32 runs an input/alarm loop and separate FreeRTOS workers for SD and
Firebase. Python serves static web assets on loopback; the browser communicates
with Firebase directly using the viewer account.

| Component | Responsibility |
| --- | --- |
| `warehouse-local.ino` | DHT22 acquisition, debounced inputs, alarm decisions, OLED and outputs, Serial commands |
| `warehouse_logic.h` | Portable state machine, hysteresis, sensor validity, door timing and acknowledgement |
| `warehouse_log.h` | Bounded sample/event CSV formatting |
| `warehouse_storage.h` | SPI/microSD worker, bounded queues, append status and loss counters |
| `warehouse_cloud.h` | Wi-Fi/authentication, token refresh, HTTP clock probes and HTTPS uploads |
| `warehouse_cloud_model.h` | Portable retry, timing, UTC and identifier policies |
| `warehouse_tls.h`, `warehouse_ca.h` | TLS profile and public Google trust anchors |
| `web/warehouse/` | Viewer login, validated telemetry, charts and CSV export |

## Timing and handoff

The DHT22 sample interval is 2.5 seconds. Sensor data becomes invalid after a failed
read or at the 6-second stale boundary; two valid reads are required for recovery.
The loop updates display/output state without running SD or network requests inline.

SD receives immutable records through a queue of depth eight. The worker owns file
operations and reports written, skipped and queue-dropped counts. It has an 8192-byte
task stack. Queue acceptance does not mean a row was successfully written.

Cloud snapshots are offered every second. The network worker schedules uploads
at a nominal 2-second firmware interval and retains only the newest pending
snapshot. Its task stack is 12288 bytes. Slow uploads replace a pending snapshot
instead of accumulating an unbounded queue. SD records are not replayed to Firebase.

These intervals are in firmware time. Simulator throttling, TLS and Internet
latency can increase the wall-clock gap between deliveries. Stack allocations are
configuration values, not measured stack high-water marks.

## Alarms and acknowledgement

Temperature uses 32°C trigger / 30°C clear thresholds; humidity uses 75% / 70% RH.
Door-open duration reaches its alarm at 10 seconds. Inputs are debounced before
state changes. Unsigned uptime differences handle timer rollover.

ACK mutes acknowledged active alarm causes for 60 seconds without clearing them.
A newly asserted cause can sound again. The dashboard shows cause and mute status
separately. A storage fault is reported independently of environmental alarm bits.

## Cloud, clocks and access

The device atomically PATCHes `latest` and one of 60 history slots over authenticated
HTTPS. UTC starts from SNTP and is corrected from verified HTTP Date headers;
upload preparation obtains a fresh Date before timestamping the latest snapshot.
Uptime continues to drive debounce, door and mute timing.

The browser streams the latest record through Firebase REST SSE, refreshes history
every 10 seconds and polls every 2 seconds as a fallback. It validates payloads,
hides readings older than 15 seconds, and flags future offsets above 30 seconds.
Charts separate boot IDs and break traces across missing or invalid measurements.

Rules grant the device UID write access and the viewer UID read access. The device
remains a trusted publisher; payload validation does not establish physical sensor
authenticity or provide sequence replay protection. See [data schema](data-schema.md).
