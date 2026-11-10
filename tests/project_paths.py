"""Paths shared by Warehouse Monitor tests; no firmware host runtime is required."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".build"
