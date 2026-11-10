"""Exercise the exact portable core bundled into the ESP32 warehouse station."""

import os
import shutil
import subprocess

import pytest

from project_paths import ROOT, BUILD

CASES = (
    "startup_recovery",
    "stale_boundary",
    "temperature_hysteresis",
    "humidity_hysteresis",
    "door_timing",
    "multiple_causes_and_bad_data",
    "mute_new_cause",
    "mute_expiry_and_clear",
    "input_bounce",
    "clock_rollover",
)


def run_portable(source_name, artifact_name, marker):
    BUILD.mkdir(exist_ok=True)
    source = ROOT / "tests/c" / source_name
    flags = [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{ROOT / 'targets/esp32-wokwi/warehouse-local'}",
    ]
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        pytest.fail("Install a native C compiler (GCC/Clang), or set CC to its executable.")
    target = BUILD / (artifact_name + (".exe" if os.name == "nt" else ""))
    subprocess.run([compiler, *flags, str(source), "-o", str(target)], check=True)
    command = [str(target)]
    # Preserve group-level results even if another group failed in the same executable.
    result = subprocess.run(command, text=True, capture_output=True, timeout=15)
    (BUILD / (artifact_name + "-output.txt")).write_text(
        result.stdout + result.stderr, encoding="utf-8"
    )
    assert result.returncode in (0, 1), result.stdout + result.stderr
    assert marker in result.stdout, result.stdout + result.stderr
    return result.stdout


@pytest.fixture(scope="module")
def logic_output():
    return run_portable("test_warehouse.c", "warehouse-logic", "WAREHOUSE_LOGIC_")


@pytest.fixture(scope="module")
def log_output():
    return run_portable("test_warehouse_log.c", "warehouse-log", "WAREHOUSE_LOG_FORMAT_")


@pytest.mark.parametrize("case", CASES)
def test_warehouse_behavior(logic_output, case):
    assert f"PASS {case}" in logic_output.splitlines(), logic_output


@pytest.mark.parametrize(
    "case",
    (
        "known_sample_vector",
        "invalid_sensor_fields",
        "signed_and_extreme_values",
        "bounded_format",
        "file_limit_boundaries",
        "short_write_detection",
        "snapshot_and_rollover",
        "event_schema",
    ),
)
def test_warehouse_log_format(log_output, case):
    assert f"PASS {case}" in log_output.splitlines(), log_output


@pytest.fixture(scope="module")
def cloud_output():
    return run_portable("test_warehouse_cloud.c", "warehouse-cloud", "WAREHOUSE_CLOUD_")


@pytest.mark.parametrize(
    "case",
    (
        "retry_bounds",
        "token_rollover",
        "identifier_bounds",
        "utc_http_date",
        "utc_token_expiry",
        "network_recovery",
        "dns_injection_route",
    ),
)
def test_warehouse_cloud_policy(cloud_output, case):
    assert f"PASS {case}" in cloud_output.splitlines(), cloud_output
