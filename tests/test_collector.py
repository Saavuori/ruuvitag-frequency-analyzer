#!/usr/bin/env python3
"""Collector tests that need no radio. Skipped where bleak is not installed.

    python -m pytest tests/ -q
"""

import os
import sys
import tempfile
import types

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

pytest.importorskip("bleak")

from webapp import protocol, store                                 # noqa: E402
from webapp.collector import Collector                             # noqa: E402


def _collector():
    c = Collector()
    c._conn = store.init(os.path.join(tempfile.mkdtemp(), "c.db"))
    return c


def _adv(mfg=None, uuids=()):
    return types.SimpleNamespace(manufacturer_data=mfg or {}, service_uuids=list(uuids),
                                 rssi=-70)


def test_only_ruuvi_devices_are_remembered():
    """Every advertiser nearby reaches the scan callback, many on rotating
    random addresses. Remembering all of them grew without bound."""
    c = _collector()
    for i in range(500):
        c.handle(types.SimpleNamespace(address=f"{i:012X}"), _adv({0x004C: b"\x02\x15"}))
    assert c._devices == {}

    df5 = bytes.fromhex("0512FC5394C37C0004FFFC040CAC364200CDCBB8334C884F")
    c.handle(types.SimpleNamespace(address="cb:b8:33:4c:88:4f"),
             _adv({protocol.RUUVI_COMPANY_ID: df5}))
    c.handle(types.SimpleNamespace(address="d6:3a:83:30:44:86"),
             _adv(uuids=[protocol.SERVICE_UUID]))
    assert set(c._devices) == {"CB:B8:33:4C:88:4F", "D6:3A:83:30:44:86"}
    assert c.stats["stored"] == 1
