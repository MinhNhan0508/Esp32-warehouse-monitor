const $ = id => document.getElementById(id);
const STALE_MS = 15000;
const POLL_MS = 2000;
const WINDOW_MS = 300000;
const storageNames = ["STARTING", "READY", "MOUNT FAILED", "I/O FAILED", "SCHEMA ERROR",
  "FILE LIMIT", "TEST FAULT", "TASK FAILED"
];
let config, session = null,
  data = null,
  timer = null,
  generation = 0;
let fetching = null,
  connected = false,
  lastError = "";
let lastFetchMs = null;
let eventStream = null,
  streamTimer = null,
  streamRenew = null,
  streamActive = false;
let diagnosticAt = -Infinity,
  diagnosticBadge = "";

function recordDelivery(r) {
  if (!validRecord(r)) return;
  const label = $("connection").textContent,
    now = performance.now();
  const fingerprint = [label, r.alarm_mask, r.sensor_valid, r.door_open, r.muted,
    r.storage_state, r.temperature_c, r.humidity_rh_pct
  ].join("|");
  if (now - diagnosticAt < 5000 && fingerprint === diagnosticBadge) return;
  diagnosticAt = now;
  diagnosticBadge = fingerprint;
  // Local, bounded, allowlisted timing only. Never send account data or tokens.
  fetch("/diagnostics", {
    method: "POST",
    headers: {
      "Content-Type": "application/json"
    },
    body: JSON.stringify({
      browser_ms: Date.now(),
      captured_at_ms: r.captured_at_ms,
      received_at_ms: r.received_at_ms,
      sequence: r.sequence,
      uptime_ms: r.uptime_ms,
      sample_age_ms: r.sample_age_ms,
      read_ms: Math.round(lastFetchMs || 0),
      boot_id: r.boot_id,
      badge: label,
      source: streamActive ? "STREAM" : "POLL",
      telemetry: {
        sensor_valid: r.sensor_valid,
        temperature_c: r.sensor_valid ? r.temperature_c : null,
        humidity_rh_pct: r.sensor_valid ? r.humidity_rh_pct : null,
        alarm_mask: r.alarm_mask,
        door_open: r.door_open,
        door_open_ms: r.door_open_ms,
        muted: r.muted,
        storage_state: r.storage_state,
        storage_samples: r.storage_samples,
        storage_skipped: r.storage_skipped,
        storage_queue_dropped: r.storage_queue_dropped,
        sensor_errors: r.sensor_errors,
        test_sensor_fault: r.test_sensor_fault
      }
    })
  }).catch(() => {});
}
const causes = mask => ["TEMPERATURE", "HUMIDITY", "DOOR OPEN TOO LONG", "SENSOR FAULT"].filter((_,
  i) => mask & (1 << i));

function ageText(timestamp, now) {
  const seconds = (now - timestamp) / 1000;
  return seconds < 0 ? `in ${(-seconds).toFixed(1)}s` : `${seconds.toFixed(1)}s ago`;
}

function authenticationError(body, status) {
  // Only expose our own messages and allowlisted codes, never arbitrary response data.
  const message = typeof body?.error?.message === "string" ? body.error.message : "";
  const code = message.split(/[\s:]/, 1)[0];
  const messages = {
    INVALID_LOGIN_CREDENTIALS: "Email or password is incorrect. Use the Firebase viewer account.",
    INVALID_PASSWORD: "Email or password is incorrect. Use the Firebase viewer account.",
    EMAIL_NOT_FOUND: "Email or password is incorrect. Use the Firebase viewer account.",
    INVALID_EMAIL: "Enter a valid email address for the Firebase viewer account.",
    USER_DISABLED: "This Firebase account is disabled. Check Authentication > Users.",
    OPERATION_NOT_ALLOWED: "Email/Password sign-in is disabled in Firebase Authentication.",
    TOO_MANY_ATTEMPTS_TRY_LATER: "Too many sign-in attempts. Wait before trying again.",
    API_KEY_INVALID: "The dashboard Firebase API key is invalid. Check project configuration.",
    API_KEY_SERVICE_BLOCKED: "The Firebase API key does not allow this authentication service.",
    API_KEY_HTTP_REFERRER_BLOCKED: "The Firebase API key does not allow this dashboard address.",
    TOKEN_EXPIRED: "Your viewer session expired. Sign in again.",
    INVALID_REFRESH_TOKEN: "Your viewer session expired. Sign in again."
  };
  if (Object.hasOwn(messages, code)) return `${messages[code]} (${code})`;
  if (message.startsWith("API key not valid"))
    return `${messages.API_KEY_INVALID} (API_KEY_INVALID)`;
  return `Firebase authentication failed (HTTP ${status}). Check the viewer account and project configuration.`;
}

async function request(url, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch(url, {
      ...options,
      signal: controller.signal,
      cache: "no-store",
      referrerPolicy: "no-referrer"
    });
    let body = null;
    try {
      body = await response.json();
    } catch {
      /* Do not surface raw response bodies. */
    }
    if (!response.ok) {
      const host = new URL(url, location.href).hostname;
      if (host === "identitytoolkit.googleapis.com" || host === "securetoken.googleapis.com")
        throw new Error(authenticationError(body, response.status));
      throw new Error(response.status === 401 || response.status === 403 ?
        "Access denied. Check account role and Database Rules." :
        `Request failed (HTTP ${response.status}).`);
    }
    return body;
  } finally {
    clearTimeout(timeout);
  }
}

function badge(text, kind = "warning") {
  $("connection").textContent = text;
  $("connection").dataset.kind = kind;
}

function validRecord(r) {
  return r && r.schema_version === 1 && /^[0-9a-f]{8}$/.test(r.boot_id) && ["sequence", "uptime_ms",
      "door_open_ms", "sample_age_ms", "sensor_errors", "storage_samples", "storage_skipped",
      "storage_queue_dropped"
    ].every(key => Number.isSafeInteger(r[key]) && r[key] >= 0 && r[key] <= 4294967295) &&
    Number.isSafeInteger(r.captured_at_ms) && Number.isSafeInteger(r.received_at_ms) &&
    typeof r.sensor_valid === "boolean" && typeof r.door_open === "boolean" && typeof r.muted ===
    "boolean" &&
    Number.isInteger(r.alarm_mask) && r.alarm_mask >= 0 && r.alarm_mask <= 15 &&
    Number.isInteger(r.storage_state) && r.storage_state >= 0 && r.storage_state <= 7 &&
    typeof r.test_sensor_fault === "boolean" &&
    (!r.sensor_valid || (Number.isFinite(r.temperature_c) && r.temperature_c >= -40 && r
      .temperature_c <= 80 &&
      Number.isFinite(r.humidity_rh_pct) && r.humidity_rh_pct >= 0 && r.humidity_rh_pct <= 100 &&
      r.sample_age_ms < 6000));
}

function history() {
  if (!validRecord(data?.latest)) return [];
  const latest = data.latest;
  return [...Object.values(data.history || {}), latest].filter(r => validRecord(r) && r.boot_id ===
      latest
      .boot_id &&
      r.captured_at_ms >= Date.now() - WINDOW_MS && r.captured_at_ms <= Date.now() + 30000)
    .sort((a, b) => a.captured_at_ms - b.captured_at_ms || a.sequence - b.sequence)
    .filter((r, i, rows) => !i || r.sequence !== rows[i - 1].sequence);
}

function svgNode(tag, attributes = {}, text = "") {
  const element = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, String(value));
  if (text) element.textContent = text;
  return element;
}

function plot(id, field, on, off) {
  const svg = $(id);
  svg.replaceChildren();
  const rows = history();
  const values = rows.filter(r => r.sensor_valid).map(r => r[field]);
  const low = Math.floor(Math.min(off - 5, ...values) / 5) * 5;
  const high = Math.ceil(Math.max(on + 5, ...values) / 5) * 5;
  const now = Date.now(),
    x = time => 44 + (time - (now - WINDOW_MS)) / WINDOW_MS * 660;
  const y = value => 180 - (value - low) / (high - low) * 160;
  for (let i = 0; i <= 4; i++) {
    const value = low + (high - low) * i / 4;
    svg.append(svgNode("line", {
      x1: 44,
      x2: 704,
      y1: y(value),
      y2: y(value),
      class: "grid"
    }));
    svg.append(svgNode("text", {
      x: 3,
      y: y(value) + 4
    }, value.toFixed(0)));
  }
  for (const threshold of [on, off]) svg.append(svgNode("line", {
    x1: 44,
    x2: 704,
    y1: y(threshold),
    y2: y(threshold),
    class: "threshold"
  }));
  let path = "",
    previous = null;
  for (const r of rows) {
    if (!r.sensor_valid) {
      previous = null;
      continue;
    }
    const move = !previous || r.captured_at_ms - previous.captured_at_ms > STALE_MS;
    path += `${move ? "M" : "L"}${x(r.captured_at_ms).toFixed(1)},${y(r[field]).toFixed(1)} `;
    svg.append(svgNode("circle", {
      cx: x(r.captured_at_ms),
      cy: y(r[field]),
      r: 2,
      class: "point"
    }));
    previous = r;
  }
  if (path) svg.append(svgNode("path", {
    d: path.trim(),
    class: "trace"
  }));
  svg.append(svgNode("text", {
    x: 44,
    y: 208
  }, "−5 min"), svgNode("text", {
    x: 675,
    y: 208
  }, "now"));
}

function render() {
  if (!session) return;
  const r = data?.latest,
    now = Date.now();
  const valid = validRecord(r);
  const age = valid ? Math.max(now - r.captured_at_ms, now - r.received_at_ms) : Infinity;
  const skew = valid && (r.captured_at_ms > now + 30000 || r.received_at_ms > now + 30000);
  const fresh = connected && valid && age <= STALE_MS && !skew;
  badge(!connected ? "CONNECTION LOST" : !valid ? "WAITING FOR DEVICE" : skew ? "CLOCK MISMATCH" :
    fresh ? "LIVE" : "STALE", fresh ? "live" : "warning");
  recordDelivery(r);
  $("temperature").textContent = fresh && r.sensor_valid ? `${r.temperature_c.toFixed(1)} °C` : "—";
  $("humidity").textContent = fresh && r.sensor_valid ? `${r.humidity_rh_pct.toFixed(1)} %` : "—";
  $("door").textContent = fresh ? (r.door_open ? "OPEN" : "CLOSED") : "—";
  $("door-duration").textContent = fresh ?
    `${Math.floor(r.door_open_ms / 1000)} seconds open · alarm after 10s` : "No fresh door data";
  $("state").textContent = !fresh ? (valid ? "STALE" : "WAITING") : r.alarm_mask ? "ALARM" : r
    .storage_state > 1 ? "STORAGE FAULT" : !r.sensor_valid || r.storage_state === 0 ? "STARTUP" :
    "NORMAL";
  $("alarm-detail").textContent = fresh ? causes(r.alarm_mask).join(" · ") ||
    "No active environmental alarm" : "Current condition is unknown";
  $("last-update").textContent = valid ?
    `Last captured ${new Date(r.captured_at_ms).toLocaleTimeString()} · ${ageText(r.captured_at_ms, now)}` :
    "Waiting for telemetry";
  $("delivery").textContent = valid ?
    `${streamActive ? "Realtime stream" : "Polling fallback"} · Snapshot ${ageText(r.captured_at_ms, now)} · Firebase receipt ${ageText(r.received_at_ms, now)} · Last read ${lastFetchMs === null ? "—" : (lastFetchMs / 1000).toFixed(1) + "s"} · Freshness limit 15s` :
    "Waiting for delivery timing";
  $("sensor").textContent = fresh ? (r.sensor_valid ? "VALID" : "UNAVAILABLE") : "—";
  $("storage").textContent = fresh ? storageNames[r.storage_state] : "—";
  $("storage-counts").textContent = fresh ?
    `${r.storage_samples} / ${r.storage_skipped} · queue drop ${r.storage_queue_dropped}` : "—";
  $("uptime").textContent = fresh ? `${Math.floor(r.uptime_ms / 1000)} s` : "—";
  $("muted").textContent = fresh ? (r.muted ? "MUTED" : r.alarm_mask ? "ACTIVE" : "OFF") : "—";
  $("boot").textContent = valid ? r.boot_id : "—";
  $("notice").textContent = lastError || (skew ?
    "Clock mismatch: a snapshot or Firebase receipt is over 30 seconds ahead of this computer. Check computer and firmware UTC clocks." :
    !fresh && valid ?
    "Historical data retained. Live values are hidden until fresh telemetry returns." : r
    ?.test_sensor_fault ? "Sensor fault injection is active in the firmware." : "");
  plot("temperature-chart", "temperature_c", 32, 30);
  plot("humidity-chart", "humidity_rh_pct", 75, 70);
  const list = $("observations");
  list.replaceChildren();
  const observations = history().filter((item, i, rows) => !i || item.alarm_mask !== rows[i - 1]
    .alarm_mask || item.door_open !== rows[i - 1].door_open || item.muted !== rows[i - 1].muted);
  for (const row of observations.slice(-8).reverse()) {
    const li = document.createElement("li"),
      time = document.createElement("time");
    time.textContent = new Date(row.captured_at_ms).toLocaleTimeString();
    li.append(time, document.createTextNode(
      `${causes(row.alarm_mask).join(", ") || "No environmental alarm"} · door ${row.door_open ? "open" : "closed"}${row.muted ? " · muted" : ""}`
    ));
    list.append(li);
  }
}
async function token() {
  if (!session) throw new Error("Sign in required.");
  if (Date.now() < session.expiresAt - 60000) return session.idToken;
  const current = session;
  const result = await request(
    `https://securetoken.googleapis.com/v1/token?key=${encodeURIComponent(config.apiKey)}`, {
      method: "POST",
      headers: {
        "Content-Type": "application/x-www-form-urlencoded"
      },
      body: new URLSearchParams({
        grant_type: "refresh_token",
        refresh_token: current.refreshToken
      })
    });
  if (session !== current) throw new Error("Session changed.");
  if (result?.user_id !== config.viewerUid || !result.id_token || !result.refresh_token)
    throw new Error("Viewer session expired. Sign in again.");
  Object.assign(current, {
    idToken: result.id_token,
    refreshToken: result.refresh_token,
    expiresAt: Date.now() + Number(result.expires_in) * 1000
  });
  return current.idToken;
}

function stopStream() {
  clearTimeout(streamTimer);
  clearTimeout(streamRenew);
  eventStream?.close();
  eventStream = null;
  streamActive = false;
}

function keepNewest(current, incoming) {
  // A GET started before a stream event must not roll the live card backward.
  return validRecord(current) && validRecord(incoming) &&
    current.received_at_ms > incoming.received_at_ms ? current : incoming;
}
async function startStream(epoch) {
  if (!session || epoch !== generation) return;
  try {
    const idToken = await token();
    if (!session || epoch !== generation) return;
    const source = new EventSource(
      `${config.databaseUrl}/devices/${encodeURIComponent(config.deviceId)}/latest.json?auth=${encodeURIComponent(idToken)}`
    );
    eventStream = source;
    const current = () => epoch === generation && eventStream === source && session;
    const retry = (delay = 5000) => {
      if (!current()) return;
      source.close();
      eventStream = null;
      streamActive = false;
      clearTimeout(streamRenew);
      streamTimer = setTimeout(() => startStream(epoch), delay);
      clearTimeout(timer);
      poll(); // Keep reading through REST while the stream reconnects.
    };
    const update = event => {
      if (!current()) return;
      try {
        if (event.data.length > 8192) throw new Error("Oversized event");
        const change = JSON.parse(event.data);
        if (change.path !== "/") throw new Error("Unexpected stream path");
        const incoming = event.type === "put" ? change.data : {
          ...data?.latest,
          ...change.data
        };
        // The firmware replaces the whole latest snapshot, so root put/patch
        // events suffice. Unsupported or malformed events fall back to GET.
        if (incoming !== null && !validRecord(incoming)) throw new Error("Invalid snapshot");
        data = {
          ...data,
          latest: keepNewest(data?.latest, incoming)
        };
        connected = true;
        streamActive = true;
        lastError = "";
        render();
      } catch {
        retry(); // Never expose raw event content or authenticated URLs.
      }
    };
    source.addEventListener("put", update);
    source.addEventListener("patch", update);
    source.addEventListener("cancel", () => retry());
    source.addEventListener("auth_revoked", () => {
      if (current()) session.expiresAt = 0;
      retry();
    });
    source.onerror = () => retry();
    // Reopen just inside token()'s refresh window. Timers and streams are closed
    // on logout; token values remain only in this page's memory.
    streamRenew = setTimeout(() => retry(0), Math.max(1000, session.expiresAt - Date.now() -
      59000));
  } catch {
    if (session && epoch === generation)
      streamTimer = setTimeout(() => startStream(epoch), 5000);
  }
}
async function poll() {
  if (fetching === generation || !session) return;
  const epoch = generation;
  const started = performance.now();
  fetching = epoch;
  try {
    const idToken = await token();
    const result = await request(
      `${config.databaseUrl}/devices/${encodeURIComponent(config.deviceId)}.json?auth=${encodeURIComponent(idToken)}`
    );
    if (epoch !== generation) return;
    data = result ? {
      ...result,
      latest: keepNewest(data?.latest, result.latest)
    } : result;
    connected = true;
    lastFetchMs = performance.now() - started;
    lastError = "";
  } catch (error) {
    if (epoch === generation) {
      connected = streamActive;
      lastError = streamActive ? "" : error.name === "AbortError" ?
        "Connection timed out. Retrying…" : error.message;
    }
  } finally {
    if (fetching === epoch) fetching = null;
    if (epoch === generation && session) {
      render();
      clearTimeout(timer);
      // Stream updates live cards immediately. GET refreshes bounded history
      // every 10 s, or serves as a 2 s fallback when the stream is unavailable.
      const interval = streamActive ? 10000 : POLL_MS;
      const delay = connected ? Math.max(250, interval - (performance.now() - started)) : 5000;
      timer = setTimeout(poll, delay);
    }
  }
}
$("login-form").addEventListener("submit", async event => {
  event.preventDefault();
  $("login").disabled = true;
  $("notice").textContent = "Signing in…";
  const epoch = ++generation;
  stopStream();
  try {
    const password = $("password").value;
    $("password").value = "";
    const result = await request(
      `https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=${encodeURIComponent(config.apiKey)}`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json"
        },
        body: JSON.stringify({
          email: $("email").value.trim(),
          password,
          returnSecureToken: true
        })
      });
    if (epoch !== generation) return;
    if (result?.localId !== config.viewerUid || !result.idToken || !result.refreshToken)
      throw new Error("Use the dashboard viewer account, not the device writer account.");
    session = {
      idToken: result.idToken,
      refreshToken: result.refreshToken,
      expiresAt: Date.now() + Number(result.expiresIn) * 1000
    };
    data = null;
    lastFetchMs = null;
    lastError = "";
    connected = false;
    $("dashboard").hidden = false;
    $("login-panel").hidden = true;
    await poll();
    startStream(epoch);
  } catch (error) {
    $("notice").textContent = error.message;
    badge("SIGN-IN FAILED", "error");
  } finally {
    $("login").disabled = !config?.apiKey;
  }
});
$("logout").addEventListener("click", () => {
  generation++;
  stopStream();
  session = null;
  data = null;
  connected = false;
  lastError = "";
  clearTimeout(timer);
  $("dashboard").hidden = true;
  $("login-panel").hidden = false;
  $("notice").textContent = "Signed out. Tokens were held only in this page's memory.";
  badge("SIGNED OUT");
});
$("refresh").addEventListener("click", () => {
  clearTimeout(timer);
  poll();
});
$("export").addEventListener("click", () => {
  const fields = ["boot_id", "sequence", "captured_at_ms", "received_at_ms", "temperature_c",
    "humidity_rh_pct", "sensor_valid", "door_open", "alarm_mask", "muted", "storage_state",
    "storage_samples", "storage_skipped"
  ];
  const rows = history();
  const csv = [fields.join(","), ...rows.map(row => fields.map(key => {
    const value = ["temperature_c", "humidity_rh_pct"].includes(key) && !row
      .sensor_valid ? "" : row[key] ?? "";
    // Only validated typed telemetry is exported; no formulas or arbitrary text fields.
    return String(value);
  }).join(","))].join("\r\n") + "\r\n";
  const url = URL.createObjectURL(new Blob([csv], {
    type: "text/csv;charset=utf-8"
  }));
  const link = document.createElement("a");
  link.href = url;
  link.download = "warehouse-history.csv";
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
setInterval(render, 1000);
try {
  config = await request("config.json");
  if (!config?.apiKey || !
    /^https:\/\/[a-z0-9-]+\.(firebaseio\.com|[a-z0-9-]+\.firebasedatabase\.app)$/.test(config
      .databaseUrl) || !config.viewerUid) throw new Error(
    "Project configuration is missing. Run the public configuration helper first.");
  $("device-id").textContent = config.deviceId;
  $("notice").textContent =
    "Sign in to read your Firebase station. No generated demo readings are shown.";
} catch (error) {
  config = null;
  $("notice").textContent = error.message;
  $("login").disabled = true;
  badge("SETUP REQUIRED");
}
