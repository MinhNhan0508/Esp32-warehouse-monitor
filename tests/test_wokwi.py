"""Check the final station wiring and the source package used in Wokwi."""

import hashlib
import importlib.util
import json
import re
import zipfile

from project_paths import ROOT


def test_warehouse_diagram_matches_firmware():
    target = ROOT / "targets/esp32-wokwi/warehouse-local"
    diagram = json.loads((target / "diagram.json").read_text(encoding="utf-8"))
    parts = {part["id"]: part for part in diagram["parts"]}
    assert len(parts) == len(diagram["parts"])
    assert parts["esp"]["attrs"]["cpuFrequency"] == "max"
    firmware = (target / "warehouse-local.ino").read_text(encoding="utf-8")
    firmware += (target / "warehouse_storage.h").read_text(encoding="utf-8")
    pins = {
        name: int(value, 0)
        for name, value in re.findall(
            r"constexpr\s+uint8_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;", firmware
        )
    }
    parent = {}

    def find(pin):
        parent.setdefault(pin, pin)
        if parent[pin] != pin:
            parent[pin] = find(parent[pin])
        return parent[pin]

    def connect(a, b):
        parent[find(a)] = find(b)

    def same(a, b):
        return find(a) == find(b)

    for a, b, *_ in diagram["connections"]:
        for endpoint in (a, b):
            assert endpoint.split(":")[0] in {*parts, "$serialMonitor"}
        connect(a, b)
    connect("esp:GND.1", "esp:GND.2")
    connect("esp:GND.1", "esp:GND.3")
    signals = {
        "dht1:SDA": "DHT_PIN",
        "oled1:SDA": "OLED_SDA",
        "oled1:SCL": "OLED_SCL",
        "sw1:2": "DOOR_PIN",
        "btn1:1.l": "ACK_PIN",
        "bz1:2": "BUZZER_PIN",
        "sd1:SCK": "SCK_PIN",
        "sd1:DO": "MISO_PIN",
        "sd1:DI": "MOSI_PIN",
        "sd1:CS": "CS_PIN",
    }
    for endpoint, constant in signals.items():
        assert same(endpoint, f"esp:{pins[constant]}"), (endpoint, constant)
    assert int(parts["oled1"]["attrs"]["i2cAddress"], 0) == pins["OLED_ADDRESS"]
    for part in ("dht1", "oled1", "sd1"):
        assert same(f"{part}:VCC", "esp:3V3")
        assert same(f"{part}:GND", "esp:GND.1")
    for endpoint in ("sw1:1", "btn1:2.l", "bz1:1", "led1:C", "led2:C"):
        assert same(endpoint, "esp:GND.1")
    endpoints = {pin for c in diagram["connections"] for pin in c[:2]}
    assert not {"sw1:3", "sd1:CD", "dht1:NC"} & endpoints
    for pin in ("DOOR_PIN", "ACK_PIN"):
        assert re.search(rf"pinMode\(\s*{pin},\s*INPUT_PULLUP\s*\)", firmware)
    for led, resistor, constant, color in (
        ("led2", "r1", "GREEN_LED_PIN", "green"),
        ("led1", "r2", "RED_LED_PIN", "red"),
    ):
        gpio = f"esp:{pins[constant]}"
        assert parts[led]["attrs"]["color"] == color
        assert int(parts[resistor]["attrs"]["value"]) == 220
        assert same(gpio, f"{resistor}:1") and same(f"{resistor}:2", f"{led}:A")
        assert not same(gpio, f"{led}:A")  # No wire bypass around the series resistor.
    assert not same("esp:3V3", "esp:GND.1")
    gpio_nets = {}
    for endpoint in endpoints:
        if re.fullmatch(r"esp:\d+", endpoint):
            gpio_nets.setdefault(find(endpoint), []).append(endpoint)
            assert not same(endpoint, "esp:3V3") and not same(endpoint, "esp:GND.1")
    assert all(len(gpios) == 1 for gpios in gpio_nets.values())


def test_bundle_uses_canonical_warehouse_source(tmp_path):
    spec = importlib.util.spec_from_file_location("wokwi_bundle", ROOT / "tools/wokwi.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    manifest = module.bundle(tmp_path)
    assert set(manifest) == {"warehouse-local"}
    with zipfile.ZipFile(tmp_path / "warehouse-local.zip") as archive:
        expected = {p.name for p in module.sources()}
        expected.remove("warehouse-local.ino")
        expected.add("sketch.ino")
        assert set(archive.namelist()) == expected
        assert "diagram.json" in expected and "libraries.txt" in expected
        assert "warehouse_tls.h" in expected
        libraries = archive.read("libraries.txt").decode().splitlines()
        assert "DHT sensor library for ESPx@1.19.0" in libraries
        assert "Adafruit SSD1306@2.5.17" in libraries
        assert "ArduinoJson@7.3.2" in libraries
        for source, expected_hash in manifest["warehouse-local"].items():
            filename = "sketch.ino" if source.endswith(".ino") else source.split("/")[-1]
            assert hashlib.sha256(archive.read(filename)).hexdigest() == expected_hash
    assert json.loads((tmp_path / "manifest.json").read_text()) == manifest
