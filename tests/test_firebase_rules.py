"""Real Firebase Auth/Realtime Database emulator checks; never contact a live project."""

import importlib.util
import os
import time

import httpx
import pytest

from project_paths import ROOT

pytestmark = pytest.mark.skipif(
    not os.environ.get("FIREBASE_DATABASE_EMULATOR_HOST"),
    reason="Run through Firebase emulators:exec",
)


@pytest.fixture(scope="module")
def firebase():
    auth_host = os.environ["FIREBASE_AUTH_EMULATOR_HOST"]
    db_host = os.environ["FIREBASE_DATABASE_EMULATOR_HOST"]
    assert auth_host.startswith("127.0.0.1:") and db_host.startswith("127.0.0.1:")
    client = httpx.Client(timeout=10)
    users = {}
    for role in ("device", "viewer", "stranger"):
        response = client.post(
            f"http://{auth_host}/identitytoolkit.googleapis.com/v1/accounts:signUp?key=fake-key",
            json={
                "email": f"{role}-{time.time_ns()}@example.test",
                "password": "LocalTestOnly-123!",
                "returnSecureToken": True,
            },
        )
        response.raise_for_status()
        users[role] = response.json()
    spec = importlib.util.spec_from_file_location(
        "warehouse_cloud_setup", ROOT / "tools/warehouse_cloud.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    base = f"http://{db_host}"
    namespace = "demo-warehouse-default-rtdb"
    response = client.put(
        f"{base}/.settings/rules.json",
        params={"ns": namespace},
        headers={"Authorization": "Bearer owner"},
        json=module.database_rules(users["device"]["localId"], users["viewer"]["localId"]),
    )
    assert response.status_code == 200, response.text

    def call(method, path, value=None, role=None):
        params = {"ns": namespace}
        if role:
            params["auth"] = users[role]["idToken"]
        return client.request(method, base + path + ".json", params=params, json=value)

    yield call
    client.close()


def payload():
    now = int(time.time() * 1000)
    return dict(
        schema_version=1,
        boot_id="aabbccdd",
        sequence=3,
        uptime_ms=8000,
        captured_at_ms=now,
        received_at_ms={".sv": "timestamp"},
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


def test_writer_patch_and_viewer_read(firebase):
    data = payload()
    result = firebase(
        "PATCH", "/devices/warehouse-01", {"latest": data, "history/0": data}, "device"
    )
    assert result.status_code == 200, result.text
    read = firebase("GET", "/devices/warehouse-01", role="viewer")
    assert read.status_code == 200 and read.json()["latest"]["temperature_c"] == 25.6
    assert isinstance(read.json()["latest"]["received_at_ms"], int)


@pytest.mark.parametrize("role", [None, "device", "stranger"])
def test_non_viewers_cannot_read(firebase, role):
    assert firebase("GET", "/devices/warehouse-01", role=role).status_code in (401, 403)


@pytest.mark.parametrize("role", [None, "viewer", "stranger"])
def test_non_writers_cannot_write(firebase, role):
    assert firebase("PUT", "/devices/warehouse-01/latest", payload(), role).status_code in (
        401,
        403,
    )


@pytest.mark.parametrize(
    "field,value",
    [
        ("humidity_rh_pct", 101),
        ("temperature_c", 81),
        ("alarm_mask", 16),
        ("schema_version", "1"),
        ("sequence", -1),
        ("sequence", 1.5),
        ("sensor_valid", "true"),
        ("unexpected", 123),
    ],
)
def test_malformed_payload_denied(firebase, field, value):
    data = payload()
    data[field] = value
    response = firebase("PUT", "/devices/warehouse-01/latest", data, "device")
    assert response.status_code in (401, 403), response.text


def test_invalid_sensor_requires_absent_values(firebase):
    data = payload()
    data["sensor_valid"] = False
    assert firebase("PUT", "/devices/warehouse-01/latest", data, "device").status_code in (401, 403)
    del data["temperature_c"]
    del data["humidity_rh_pct"]
    assert firebase("PUT", "/devices/warehouse-01/latest", data, "device").status_code == 200


def test_stale_timestamp_and_delete_denied(firebase):
    data = payload()
    data["captured_at_ms"] -= 120000
    assert firebase("PUT", "/devices/warehouse-01/latest", data, "device").status_code in (401, 403)
    assert firebase("DELETE", "/devices/warehouse-01/latest", role="device").status_code in (
        401,
        403,
    )


def test_history_boundaries_and_other_device(firebase):
    assert (
        firebase("PUT", "/devices/warehouse-01/history/59", payload(), "device").status_code == 200
    )
    assert firebase("PUT", "/devices/warehouse-01/history/60", payload(), "device").status_code in (
        401,
        403,
    )
    assert firebase("PUT", "/devices/warehouse-02/latest", payload(), "device").status_code in (
        401,
        403,
    )
