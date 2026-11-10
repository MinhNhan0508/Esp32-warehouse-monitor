# W06 cloud data contract — schema version 1

Transport uses authenticated HTTPS REST. A successful firmware upload
atomically PATCHes `/devices/warehouse-01.json` with `latest` and `history/N`
containing the same snapshot. N cycles through 0–59 after successful writes.
The firmware accepts HTTP 200/204 and counts completed uploads, not queue entries.

| Field | Type / unit / meaning |
| --- | --- |
| `schema_version` | Integer 1 |
| `boot_id` | Eight lowercase hex characters; random 32-bit boot label, not globally unique |
| `sequence` | uint32 snapshot counter, separate from SD record sequence; gaps are expected |
| `uptime_ms` | uint32 firmware millis at snapshot capture |
| `captured_at_ms` | Unix epoch milliseconds, synchronized UTC minus snapshot age |
| `received_at_ms` | Firebase server timestamp, submitted as `{ ".sv": "timestamp" }` |
| `sensor_valid` | Boolean; startup/recovery and stale checks already applied |
| `temperature_c` | Number, degrees Celsius, −40…80; absent if invalid |
| `humidity_rh_pct` | Number, relative humidity percent, 0…100; absent if invalid |
| `sample_age_ms` | uint32 since last good DHT acquisition; UINT32_MAX before any good sample; <6000 when valid |
| `door_open` | Boolean debounced door input |
| `door_open_ms` | uint32 continuous open duration at capture |
| `alarm_mask` | Integer 0…15: temperature=1, humidity=2, door=4, sensor=8 |
| `muted` | Boolean environmental-alarm acknowledgement interval active |
| `sensor_errors` | uint32 failed/rejected acquisition count |
| `storage_state` | 0 starting, 1 ready, 2 mount failed, 3 I/O failed, 4 header/schema failed, 5 file limit, 6 injected fault, 7 task failed |
| `storage_samples` | uint32 sample rows reported fully written by SD worker |
| `storage_skipped` | uint32 records intentionally skipped while storage unavailable |
| `storage_queue_dropped` | uint32 rejected record/command submissions when input queue full |
| `test_sensor_fault` | Boolean explicit software fault injection active |

Invalid temperature/humidity are omitted, never replaced with zero. No passwords,
emails, ID tokens or refresh tokens are part of this payload. `captured_at_ms`
describes a **status snapshot**; `sample_age_ms` tells how old its underlying DHT
measurement was at capture. `received_at_ms` does not imply a new sensor reading.

UTC is initialized by SNTP (15 s update interval in simulated time), then corrected
using valid HTTP Date headers from certificate/hostname-verified Google/Firebase
HTTPS responses, including rejected uploads. Date has one-second resolution and
network delay; this is not a precision clock. Sensor age, alarm timers and uptime
continue to use monotonic firmware milliseconds, unaffected by UTC corrections.
Each upload obtains a verified HTTP Date with HEAD on the database origin,
then builds the newest snapshot and sends PATCH over the retained TLS connection.
The capture estimate uses that explicit server date minus monotonic snapshot age,
rather than relying only on the simulator's advancing UTC clock. Missing dates or
a closed clock-probe connection abort the upload. A HEAD 405 with valid Date is
not a PATCH failure.
Token refresh checks both firmware elapsed time and the UTC expiry deadline.
Pausing or throttling the simulator can still interrupt timely cloud delivery;
the dashboard must continue to mark old data stale.

Rules reject unknown fields, malformed types/ranges, missing required values,
unacceptable timestamps, unauthorized roles and client deletion. The device user
is still a trusted publisher: these Rules cannot prove a reading came from a
physical sensor, and do not provide monotonic/replay protection for the sequence.
No such hardware authenticity claim is made.

The dashboard streams the latest snapshot through Firebase REST SSE, refreshes
history every 10 s, and polls every 2 s as a fallback. It marks snapshots older
than 15 s stale, flags a
future clock offset above 30 s, validates received types and isolates boot IDs in
charts. Missing intervals above 15 s and invalid sensor rows break chart traces.
Only the current boot's retained five-minute history is exported. A reboot starts
the ring index again; old slots are overwritten progressively and filtered by boot.

This is a bounded lab history, not a production retention policy. `latest` is not
a transaction log; a brief alarm between uploads can be absent from cloud history.
Use the SD event log when inspecting local events and note its own loss counters.
