"""Public configuration generation must agree across firmware, Rules and web."""

import importlib.util
import json

import pytest

from project_paths import ROOT

spec = importlib.util.spec_from_file_location("cloud_setup", ROOT / "tools/warehouse_cloud.py")
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


def test_generated_public_files_agree(tmp_path):
    config = setup.identifiers(
        "PUBLIC_API_KEY_123456789",
        "https://example-default-rtdb.asia-southeast1.firebasedatabase.app/",
        "DEVICE_UID",
        "VIEWER_UID",
    )
    header, rules, web = setup.configure(config, tmp_path)
    assert json.loads(web.read_text()) == config
    assert config["databaseUrl"] in header.read_text()
    device = json.loads(rules.read_text())["rules"]["devices"]["warehouse-01"]
    assert "VIEWER_UID" in device[".read"] and "DEVICE_UID" in device["latest"][".write"]
    assert "password" not in header.read_text().lower()
    assert "WAREHOUSE_CLOUD_ENABLED 1" in header.read_text()


@pytest.mark.parametrize(
    "field,value",
    [
        ("api_key", "bad key"),
        ("database_url", "http://example.firebaseio.com"),
        ("database_url", "https://example.firebaseio.com.evil.test"),
        ("database_url", "https://user:password@example.firebaseio.com"),
        ("database_url", "https://example.firebaseio.com/devices"),
        ("database_url", "https://example.firebaseio.com?auth=token"),
        ("device_uid", "a/b"),
        ("viewer_uid", "DEVICE"),
    ],
)
def test_reject_incorrect_public_configuration(field, value):
    args = dict(
        api_key="PUBLIC_API_KEY_123456789",
        database_url="https://example.firebaseio.com",
        device_uid="DEVICE",
        viewer_uid="VIEWER",
    )
    args[field] = value
    with pytest.raises(ValueError):
        setup.identifiers(**args)
