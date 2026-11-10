"""Browser UI checks with explicitly mocked Firebase traffic; not live cloud evidence."""

import importlib.util
import json
import os
from pathlib import Path
import threading
import time

import pytest
from playwright.sync_api import sync_playwright, expect

from project_paths import ROOT, BUILD

CONFIG = dict(
    apiKey="DEMO_PUBLIC_KEY",
    databaseUrl="https://demo-warehouse-default-rtdb.asia-southeast1.firebasedatabase.app",
    viewerUid="VIEWER",
    deviceUid="DEVICE",
    deviceId="warehouse-01",
)


def frame():
    return dict(
        schema_version=1,
        boot_id="aabbccdd",
        sequence=3,
        uptime_ms=8000,
        captured_at_ms=int(time.time() * 1000),
        received_at_ms=int(time.time() * 1000),
        sensor_valid=True,
        temperature_c=25.6,
        humidity_rh_pct=64.5,
        sample_age_ms=1000,
        door_open=False,
        door_open_ms=0,
        alarm_mask=0,
        muted=False,
        sensor_errors=0,
        storage_state=1,
        storage_samples=3,
        storage_skipped=0,
        storage_queue_dropped=0,
        test_sensor_fault=False,
    )


@pytest.fixture(scope="module")
def dashboard_url():
    spec = importlib.util.spec_from_file_location(
        "warehouse_dashboard_server", ROOT / "tools/warehouse_cloud.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    http = module.server(0)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{http.server_port}"
    http.shutdown()
    http.server_close()
    thread.join(timeout=5)


@pytest.fixture(scope="module")
def browser():
    os.environ.setdefault("PLAYWRIGHT_BROWSERS_PATH", str(ROOT / ".tools/playwright"))
    with sync_playwright() as p:
        options = {"headless": True}
        chrome = Path("C:/Program Files/Google/Chrome/Application/chrome.exe")
        if chrome.is_file():
            options["executable_path"] = str(chrome)
        result = p.chromium.launch(**options)
        yield result
        result.close()


@pytest.fixture
def station(browser, dashboard_url):
    page = browser.new_page(viewport={"width": 1440, "height": 1000})
    current = frame()
    state = {
        "data": {"latest": current, "history": {"0": current}},
        "offline": False,
        "uid": "VIEWER",
        "expires": "3600",
        "refreshes": 0,
    }
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.route("**/config.json", lambda route: route.fulfill(json=CONFIG))
    page.route(
        "https://identitytoolkit.googleapis.com/**",
        lambda route: route.fulfill(
            json={
                "localId": state["uid"],
                "idToken": "mock-id-token",
                "refreshToken": "mock-refresh",
                "expiresIn": state["expires"],
            }
        ),
    )

    def refresh(route):
        state["refreshes"] += 1
        route.fulfill(
            json={
                "user_id": "VIEWER",
                "id_token": "mock-refreshed-id",
                "refresh_token": "mock-refreshed-token",
                "expires_in": "3600",
            }
        )

    page.route("https://securetoken.googleapis.com/**", refresh)

    def database(route):
        if state["offline"]:
            route.abort()
        else:
            route.fulfill(json=state["data"])

    page.route("https://*.firebasedatabase.app/**", database)
    page.goto(dashboard_url)
    expect(page.locator("#notice")).to_contain_text("Sign in to read")
    yield page, state, errors
    assert not errors
    page.close()


def login(page):
    page.locator("#email").fill("viewer@example.test")
    page.locator("#password").fill("LocalTestOnly-123!")
    page.locator("#login").click()


def test_live_values_export_and_mobile(station):
    page, state, _ = station
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    expect(page.locator("#temperature")).to_have_text("25.6 °C")
    expect(page.locator("#state")).to_have_text("NORMAL")
    assert page.evaluate("localStorage.length === 0 && sessionStorage.length === 0")
    artifacts = BUILD / "warehouse-browser"
    artifacts.mkdir(parents=True, exist_ok=True)
    page.screenshot(path=str(artifacts / "mock-dashboard-desktop.png"), full_page=True)
    with page.expect_download() as download:
        page.locator("#export").click()
    output = artifacts / "mock-history.csv"
    download.value.save_as(str(output))
    assert "25.6" in output.read_text(encoding="utf-8")
    page.set_viewport_size({"width": 390, "height": 844})
    page.screenshot(path=str(artifacts / "mock-dashboard-mobile.png"), full_page=True)
    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")


def test_local_delivery_diagnostics_are_bounded_and_reject_extra_fields(station):
    page, _, _ = station
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    response = page.request.get(page.url + "diagnostics")
    assert response.ok
    records = response.json()["records"]
    assert records and records[-1]["badge"] == "LIVE"
    record = records[-1]
    assert record["telemetry"]["sensor_valid"] is True
    assert record["telemetry"]["temperature_c"] == 25.6
    for change in (
        {"password": "REJECT"},
        {"temperature_c": "25.6"},
        {"sensor_valid": False},
        {"alarm_mask": True},
        {"storage_samples": 4294967296},
    ):
        invalid = {**record, "telemetry": {**record["telemetry"], **change}}
        assert page.request.post(page.url + "diagnostics", data=invalid).status == 400
    for _ in range(125):
        assert page.request.post(page.url + "diagnostics", data=record).status == 204
    assert len(page.request.get(page.url + "diagnostics").json()["records"]) == 120
    assert (
        page.request.post(page.url + "diagnostics", data={**record, "password": "REJECT"}).status
        == 400
    )
    assert (
        page.request.post(
            page.url + "diagnostics", data=record, headers={"Origin": "https://unrelated.example"}
        ).status
        == 403
    )
    assert (
        page.request.post(
            page.url + "diagnostics", data="x" * 1025, headers={"Content-Type": "application/json"}
        ).status
        == 400
    )


def test_native_firebase_stream_put_patch_and_no_poll_rollback(station):
    page, state, _ = station
    record = {**state["data"]["latest"], "sequence": 101, "temperature_c": 34.0, "alarm_mask": 1}
    record["received_at_ms"] += 1000
    events = "event: put\ndata: " + json.dumps({"path": "/", "data": record}) + "\n\n"
    events += (
        "event: patch\ndata: "
        + json.dumps({"path": "/", "data": {"humidity_rh_pct": 80.0, "alarm_mask": 3}})
        + "\n\n"
    )
    page.route(
        "https://*.firebasedatabase.app/**/latest.json?*",
        lambda route: route.fulfill(content_type="text/event-stream", body=events),
    )
    login(page)
    expect(page.locator("#temperature")).to_have_text("34.0 °C")
    expect(page.locator("#humidity")).to_have_text("80.0 %")
    page.locator("#refresh").click()
    expect(page.locator("#temperature")).to_have_text("34.0 °C")
    expect(page.locator("#humidity")).to_have_text("80.0 %")


def test_stream_updates_before_poll_and_closes_on_logout(station):
    page, state, _ = station
    page.add_init_script("""
      window.EventSource = class extends EventTarget {
        constructor(url) { super(); this.url = url; this.closed = false; window.testStream = this; }
        close() { this.closed = true; }
      };
    """)
    page.reload()
    page.clock.install()
    for key in ("captured_at_ms", "received_at_ms"):
        state["data"]["latest"][key] -= 14000
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    assert page.evaluate("Boolean(window.testStream)")
    record = frame()
    record.update(
        sequence=102,
        captured_at_ms=page.evaluate("Date.now()"),
        received_at_ms=page.evaluate("Date.now()"),
        temperature_c=34.0,
        alarm_mask=1,
    )
    page.evaluate(
        "r => testStream.dispatchEvent(new MessageEvent('put', {data: JSON.stringify({path: '/', data: r})}))",
        record,
    )
    expect(page.locator("#temperature")).to_have_text("34.0 °C")
    expect(page.locator("#delivery")).to_contain_text("Realtime stream")
    with page.expect_response(
        lambda r: r.url.endswith("/diagnostics") and r.request.method == "POST"
    ) as diagnostic:
        page.clock.run_for(5500)
    assert diagnostic.value.status == 204
    expect(page.locator("#connection")).to_have_text("LIVE")
    diagnostics = page.request.get(page.url + "diagnostics").json()["records"]
    assert any(r.get("source") == "STREAM" and r["sequence"] == 102 for r in diagnostics)
    page.locator("#logout").click()
    assert page.evaluate("testStream.closed")
    page.evaluate(
        "r => testStream.dispatchEvent(new MessageEvent('put', {data: JSON.stringify({path: '/', data: r})}))",
        record,
    )
    expect(page.locator("#connection")).to_have_text("SIGNED OUT")
    expect(page.locator("#dashboard")).to_be_hidden()


def test_door_ack_and_storage_fault_recovery(station):
    page, state, _ = station
    record = state["data"]["latest"]
    record.update(door_open=True, door_open_ms=12000, alarm_mask=4)
    login(page)
    expect(page.locator("#door")).to_have_text("OPEN")
    expect(page.locator("#alarm-detail")).to_contain_text("DOOR OPEN TOO LONG")
    expect(page.locator("#muted")).to_have_text("ACTIVE")
    record["muted"] = True
    page.locator("#refresh").click()
    expect(page.locator("#muted")).to_have_text("MUTED")
    expect(page.locator("#state")).to_have_text("ALARM")
    record.update(door_open=False, door_open_ms=0, alarm_mask=0, muted=False, storage_state=6)
    page.locator("#refresh").click()
    expect(page.locator("#state")).to_have_text("STORAGE FAULT")
    expect(page.locator("#storage")).to_have_text("TEST FAULT")
    expect(page.locator("#temperature")).to_have_text("25.6 °C")
    record["storage_state"] = 1
    page.locator("#refresh").click()
    expect(page.locator("#state")).to_have_text("NORMAL")
    expect(page.locator("#storage")).to_have_text("READY")


def test_stale_data_hidden_and_reconnect(station):
    page, state, _ = station
    for key in ("captured_at_ms", "received_at_ms"):
        state["data"]["latest"][key] -= 60000
    login(page)
    expect(page.locator("#connection")).to_have_text("STALE")
    expect(page.locator("#temperature")).to_have_text("—")
    latest = frame()
    state["data"] = {"latest": latest, "history": {"0": latest}}
    page.locator("#refresh").click()
    expect(page.locator("#connection")).to_have_text("LIVE")
    state["offline"] = True
    page.locator("#refresh").click()
    expect(page.locator("#connection")).to_have_text("CONNECTION LOST")
    expect(page.locator("#temperature")).to_have_text("—")
    state["offline"] = False
    page.locator("#refresh").click()
    expect(page.locator("#connection")).to_have_text("LIVE")


@pytest.mark.parametrize("old_field", ("captured_at_ms", "received_at_ms"))
def test_fresh_read_does_not_mask_old_snapshot_or_receipt(station, old_field):
    page, state, _ = station
    state["data"]["latest"][old_field] -= 20000
    login(page)
    expect(page.locator("#connection")).to_have_text("STALE")
    expect(page.locator("#temperature")).to_have_text("—")
    expect(page.locator("#delivery")).to_contain_text("Firebase receipt")
    expect(page.locator("#delivery")).to_contain_text("Freshness limit 15s")
    # Reading the same record again must not make it appear newly captured.
    page.locator("#refresh").click()
    expect(page.locator("#connection")).to_have_text("STALE")


def test_automatic_poll_recovers_with_new_snapshot(station):
    page, state, _ = station
    page.clock.install()
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    page.clock.run_for(16000)
    expect(page.locator("#connection")).to_have_text("STALE")
    expect(page.locator("#temperature")).to_have_text("—")
    now = page.evaluate("Date.now()")
    latest = frame()
    latest.update(
        captured_at_ms=now,
        received_at_ms=now,
        sequence=4,
        temperature_c=34.0,
        alarm_mask=1,
    )
    state["data"] = {"latest": latest, "history": {"0": latest}}
    page.clock.run_for(2100)
    expect(page.locator("#connection")).to_have_text("LIVE")
    expect(page.locator("#temperature")).to_have_text("34.0 °C")
    expect(page.locator("#state")).to_have_text("ALARM")


def test_invalid_sensor_and_alarm(station):
    page, state, _ = station
    r = state["data"]["latest"]
    r.update(sensor_valid=False, alarm_mask=8, test_sensor_fault=True)
    r.pop("temperature_c")
    r.pop("humidity_rh_pct")
    login(page)
    expect(page.locator("#state")).to_have_text("ALARM")
    expect(page.locator("#temperature")).to_have_text("—")
    expect(page.locator("#sensor")).to_have_text("UNAVAILABLE")
    expect(page.locator("#notice")).to_contain_text("fault injection")


def test_device_account_rejected_and_logout(station):
    page, state, _ = station
    state["uid"] = "DEVICE"
    login(page)
    expect(page.locator("#notice")).to_contain_text("viewer account")
    expect(page.locator("#dashboard")).to_be_hidden()
    state["uid"] = "VIEWER"
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    page.locator("#logout").click()
    expect(page.locator("#dashboard")).to_be_hidden()
    expect(page.locator("#connection")).to_have_text("SIGNED OUT")


def test_chart_gaps_and_boot_isolation(station):
    page, state, _ = station
    now = int(time.time() * 1000)
    rows = []
    for i, age in enumerate((120000, 115000, 110000, 30000, 25000)):
        r = frame()
        r.update(sequence=i, captured_at_ms=now - age, received_at_ms=now - age)
        rows.append(r)
    rows[2]["sensor_valid"] = False
    rows[2].pop("temperature_c")
    rows[2].pop("humidity_rh_pct")
    foreign = frame()
    foreign["boot_id"] = "11223344"
    state["data"]["history"] = {str(i): r for i, r in enumerate(rows + [foreign])}
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    path = page.locator("#temperature-chart .trace").get_attribute("d")
    assert path.count("M") == 3
    assert page.locator("#temperature-chart .point").count() == 5


def test_token_refresh(station):
    page, state, _ = station
    state["expires"] = "60"
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")
    assert state["refreshes"] == 1


def test_clock_mismatch_hides_values(station):
    page, state, _ = station
    for key in ("captured_at_ms", "received_at_ms"):
        state["data"]["latest"][key] += 120000
    login(page)
    expect(page.locator("#connection")).to_have_text("CLOCK MISMATCH")
    expect(page.locator("#temperature")).to_have_text("—")
    expect(page.locator("#last-update")).to_contain_text("in ")
    expect(page.locator("#delivery")).to_contain_text("Firebase receipt in ")
    expect(page.locator("#notice")).to_contain_text("Check computer and firmware UTC clocks")


def test_malformed_telemetry_rejected(station):
    page, state, _ = station
    state["data"]["latest"]["storage_samples"] = "=1+1"
    login(page)
    expect(page.locator("#connection")).to_have_text("WAITING FOR DEVICE")
    expect(page.locator("#temperature")).to_have_text("—")


def test_failed_login_explains_error_and_allows_retry(station):
    page, _, _ = station
    pattern = "https://identitytoolkit.googleapis.com/**"

    def reject(route):
        route.fulfill(status=400, json={"error": {"message": "INVALID_LOGIN_CREDENTIALS"}})

    page.route(pattern, reject)
    login(page)
    expect(page.locator("#notice")).to_contain_text("Email or password is incorrect")
    expect(page.locator("#notice")).to_contain_text("INVALID_LOGIN_CREDENTIALS")
    expect(page.locator("#dashboard")).to_be_hidden()
    expect(page.locator("#login")).to_be_enabled()
    expect(page.locator("#password")).to_have_value("")
    page.unroute(pattern, reject)
    login(page)
    expect(page.locator("#connection")).to_have_text("LIVE")


@pytest.mark.parametrize(
    "server_message,expected",
    [
        ("OPERATION_NOT_ALLOWED", "Email/Password sign-in is disabled"),
        ("TOO_MANY_ATTEMPTS_TRY_LATER", "Wait before trying again"),
        ("API key not valid. Please pass a valid API key.", "API_KEY_INVALID"),
        (
            "UNRECOGNIZED_SERVER_RESPONSE mock-private-detail",
            "Firebase authentication failed",
        ),
    ],
)
def test_auth_errors_are_actionable_without_echoing_raw_response(station, server_message, expected):
    page, _, _ = station
    page.route(
        "https://identitytoolkit.googleapis.com/**",
        lambda route: route.fulfill(status=400, json={"error": {"message": server_message}}),
    )
    login(page)
    expect(page.locator("#notice")).to_contain_text(expected)
    assert "mock-private-detail" not in page.locator("#notice").inner_text()
    expect(page.locator("#dashboard")).to_be_hidden()
