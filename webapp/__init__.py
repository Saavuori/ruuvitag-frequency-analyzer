"""RuuviTag frequency analyzer - host side.

    protocol.py   BLE wire formats: DF5, 0xC2 broadcast spectra, GATT stream
    dsp.py        STFT, band metrics, peak detection
    store.py      SQLite
    collector.py  radio: passive scan and connected capture
    server.py     HTTP API and the UI

Nothing here imports anything from `firmware/`; the only thing shared between
the two halves is the wire format, and it is written down twice on purpose -
once in C and once in Python - with tests that hold the two to the same bytes.
"""

__version__ = "0.1.0"
