# CLAUDE.md

Project guidance for Claude Code. Read this before making changes.

## What this is

A **1–50 Hz vibration spectrum analyzer** built on RuuviTag hardware
(nRF52832 + LIS2DH12). Custom Zephyr firmware streams raw 400 Hz accelerometer
samples over BLE; a local Python service transforms them and a browser draws a
live spectrum and waterfall.

The firmware **has** been built and run on hardware. `docs/` describes what it
actually does, with measured numbers.

## Hard rules

1. **Numbers in docs are measured, or marked as not measured.** No placeholders
   that look like results. If you add a figure, say where it came from. The
   measured ODR (379.7 Hz against 400 nominal) is the reason this matters.
2. **Never splice or zero-fill a gap.** Lost samples get NaN and are drawn as
   holes. Splicing compresses time, which reads as a frequency error; zero-fill
   inserts a broadband click. Both produce artefacts indistinguishable from
   real measurements.
3. **Never name a tone that has not cleared the prominence test.** "No tonal
   peak" is a real answer and must stay available.
4. **DF5 output is byte-compatible or it is broken** (C-1). `webapp/protocol.py`
   is checked against Ruuvi's published vectors; keep it that way.
5. **This is not a medical or safety device** (C-2). It measures vibration.
6. **The tag must never go permanently silent** (P-3). Two designs failed this;
   read [ADR-0004](docs/adr/0004-advertising-is-reasserted-every-slot.md) before
   touching advertising.

## Conventions

- Requirements are numbered in `docs/02-requirements.md` (F-, S-, P-, C-, O-).
  Cite them in commits and test names: `fix(dsp): coherent-gain amplitude (S-1)`.
- Non-obvious decisions become ADRs in `docs/adr/`, numbered sequentially,
  following the existing Context / Decision / Rationale / Consequences shape.
- `docs/` and the code must agree. When they diverge, fix both in one commit.
- Firmware: Zephyr style, `snake_case`, `rfa_` prefix, `RFA_` for macros.
- Host: strict downward layering. `protocol` imports nothing of ours and stays
  numpy-free; `dsp` does no I/O; `store` does not import `collector`.

## Amplitude calibration — read before touching `dsp.py` or `fft.c`

Bins use **coherent gain**, `A = 2|X| / Σw`, so a bin reads the amplitude of a
tone sitting on it. Both implementations must match, and
`tests/test_dsp.py` pins them to planted tones.

The Parseval form `sqrt(4|X|²/(N·Σw²))` is correct only when summed across the
whole main lobe. Used per bin it reads an on-centre tone **18% low**. This bug
was in both copies. Do not reintroduce it.

Band levels then need the ENBW division, or a single tone reads 22% high.

## Commands

```bash
python run.py --stream AA:BB:CC:DD:EE:FF   # capture and serve on :8765
python run.py --no-collect                 # serve stored data only
python tools/fake_source.py --duration 280 # synthetic scene, no hardware
python tests/test_dsp.py                   # or: python -m pytest tests/ -q
```

Firmware (Zephyr 4.4.1, board `ruuvi_ruuvitag`):

```bash
west build -p always -b ruuvi_ruuvitag -d build-rfa firmware
nrfjprog --program build-rfa/zephyr/zephyr.hex --chiperase --verify --reset
```

`--chiperase` removes Ruuvi's DFU bootloader; SWD becomes the only way in.
Confirm you are talking to the tag and not a DK's own nRF52832 first —
`docs/07-build-and-flash.md` has the FICR reads for that.

## What you cannot verify from here

Whether a J-Link enumerated, whether the tag is seated, what the tag is actually
attached to, and whether a spectrum looks like the machine someone thinks it is.
State these as things the user must check.

**Do not test "is the tag advertising?" by scanning after a disconnect.**
Windows suppresses a recently-disconnected peer from scan results, which reads
as a silent tag when it is not. Probe by reconnecting instead.
