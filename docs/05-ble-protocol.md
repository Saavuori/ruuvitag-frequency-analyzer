# 05 — BLE protocol

Three channels, all under Ruuvi's company ID `0x0499` except the GATT service.

| Channel | Kind | Carries | Rate |
|---------|------|---------|------|
| DF5 `0x05` | advertisement | environment + DC accel | one per ~2.6 s |
| `0xC2` | advertisement | 128-bin spectrum, chunked | one frame per ~10 s |
| GATT stream | connection | raw 400 Hz samples | ~2.4 kB/s |

A parser must **switch on the format byte**. Ruuvi's company ID carries several
products; a parser that assumes every `0x0499` payload is DF5 decodes a `0xC2`
chunk as a nonsense temperature and a plausible-looking pressure. Our own
scanner heard a `0xE1` device (Ruuvi Air) on the bench and correctly rejected it.

---

## Channel A — Data Format 5 (`0x05`)

Emitted **unmodified**, byte-compatible with the Ruuvi ecosystem. Ruuvi Station,
Ruuvi Gateway and the Home Assistant integration work against a tag running this
firmware without modification. `webapp/protocol.py` is checked against Ruuvi's
published valid/max/min/invalid vectors in `tests/test_protocol.py`.

Two fields deserve comment:

**The acceleration triplet is a ~20 s exponential average of gravity**, sampled
once per advertisement. It shows orientation and gross displacement. It is not
a movement waveform and cannot represent anything above ~0.2 Hz — which is
below this project's entire band. Do not plot it next to a spectrum and expect
them to agree.

**The movement counter stays at 0.** In Ruuvi's firmware it counts
threshold-crossing events. This firmware measures spectra, not events, and
inventing a threshold to populate the field would put a number nobody chose in
front of consumers who would reasonably believe it.

---

## Channel B — spectrum frames `0xC2`

The connectionless view: a whole 0–50 Hz spectrum, split across advertisements.

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| 0 | uint8 | Format | `0xC2` |
| 1 | uint8 | Version + flags | bits 7–4 = spec version (`0x1`); bit 0 = data invalid |
| 2 | uint8 | Frame ID | wraps; identifies which spectrum a chunk belongs to |
| 3 | uint8 | Chunk index | 0 … chunk count − 1 |
| 4 | uint8 | Chunk count | 8 at version 1 |
| 5 | uint8 | First bin | index of the first bin in this chunk |
| 6–23 | uint8 × 18 | Bin magnitudes | 0.5 dB/LSB referenced to 1 µg |

### Why these choices

**No MAC.** Unlike DF5, the payload carries no address. The BLE advertisement
header already has one and that is what a scanner joins on; those six bytes buy
a third of another chunk.

**The bin scale is fixed by spec version, not transmitted.** Six header bytes
already cost a third of a chunk. Version 1 is a 256-point transform at the
100 Hz decimated rate: **0.390625 Hz per bin, 128 bins, 0 to 49.6 Hz**.

**Logarithmic magnitudes.** A spectrum spans the noise floor to a hard knock —
five orders of magnitude. A linear byte would quantise the quiet end into two or
three distinct values. 0.5 dB/LSB from 1 µg reaches 2.37 g in one byte per bin.

**Partial frames must be discarded, not padded.** A half-heard spectrum rendered
as a waterfall column shows a band of silence that never happened; a visible gap
is the honest alternative. `SpectrumAssembler` only releases frames heard in
full.

### What it costs, measured

Eight advertisements per frame. At the 1.28 s slot that is one spectrum per
10.24 s **before** loss. On the bench, 30 s of listening at −84 dBm heard chunks
`[0,2,3,4,5,6]` and reassembled **zero** complete frames.

That is the honest characterisation of this channel, and the reason ADR-0002
adds a second one. Use `0xC2` to answer "is anything happening in that room".
Do not use it to look at a machine.

---

## Channel C — the GATT sample stream

The instrument. Advertised in the scan response so an active scan can filter for
tags that support it.

```
Service  f1a70001-9c3f-4f5a-8b21-2d6a3c9e7d10
  Samples  f1a70002-…   notify   raw AC samples, gravity removed
  Control  f1a70003-…   write    0x01 start, 0x00 stop, 0x02 <u16> threshold
  Info     f1a70004-…   read     rates and units
  Stats    f1a70005-…   read     duty cycle, counts, battery
```

### Samples notification

| Offset | Type | Field |
|--------|------|-------|
| 0 | uint8 | bits 7–4 protocol version (`1`), bit 0 = gap before this packet |
| 1 | uint8 | sample count *n* |
| 2–5 | uint32 LE | index of the first sample, monotonic |
| 6… | int16 LE × 3*n* | x, y, z per sample, mg |

**The index is the whole design.** A host that receives index 4000 after index
3000 with 39 samples knows exactly how many samples are missing and leaves a
hole of that width. Splicing the gap shut would shift every later sample in
time, and a time shift is a frequency error — the one error a spectrum analyser
must never introduce silently.

**Samples are AC**, with the tracked DC (gravity) estimate already subtracted.
Streaming DC would put ~1000 mg in bin 0 and, through window leakage, in bins 1
and 2 as well — 0.39 to 0.78 Hz, inside the band of interest.

Samples per notification is derived from the negotiated MTU, per packet rather
than latched at connect: MTU exchange is initiated by the central and may land
after the first notification. Assuming the large MTU too early would truncate
silently and corrupt sample alignment for the rest of the connection.

### Info characteristic

| Offset | Type | Field |
|--------|------|-------|
| 0 | uint8 | protocol version |
| 1 | uint8 | axis count (3) |
| 2–5 | uint32 LE | nominal ODR, milli-Hz |
| 6–9 | uint32 LE | measured ODR, milli-Hz, 0 if not yet known |
| 10 | uint8 | mg per int16 LSB |
| 11 | uint8 | full scale, g |

**Read this. Do not assume 400 Hz.** The LIS2DH12's ODR comes from an RC
oscillator. Measured on the bench tag:

```
nominal 400.000 Hz    measured 379.658 Hz    −5.1%
```

Take the label and every frequency the analyser draws is 5.4% high: a 50 Hz
component reads as 52.7 Hz, and mains hum lands somewhere it cannot be
recognised. The host scales by the measured rate and the UI marks the
difference between "measured" and "assumed".

### Stats characteristic

22 bytes, little-endian: uptime (u32 s), then milliseconds idle / bursting /
streaming (u32 each), then burst count, motion count and battery millivolts
(u16 each). Battery 0 means the ADC could not be read — not a flat cell.

This exists because battery life cannot be measured on this bench. A CR2477
barely moves in voltage for months, so a voltage trend can contradict a
multi-year estimate but never confirm one. Reporting occupancy instead makes the
power figure arithmetic over numbers the tag actually observed; only the
per-mode currents are taken on trust. See [08 Power](08-power.md).

### Control command 0x02 — motion threshold

`0x02` followed by a little-endian `uint16` of milligravity re-arms the activity
interrupt at that threshold. **Not persisted**: a reboot returns to
`CONFIG_RFA_MOTION_THRESHOLD_MG`. It is a tuning aid, because the right
threshold depends on what the tag is bolted to and finding it by reflashing is
miserable. `tools/tune_motion.py` drives it.

### Measured behaviour

30 s capture from the bench tag:

```
742 packets → 11416 samples = 377.9 Hz effective
0 gap flags, 0 samples lost by index
2.36 kB/s
17 samples per notification (MTU negotiated below the 247 requested)
```

### Advertising while connected

**Advertising stops for the duration of a capture**, so DF5 and `0xC2` both
pause and Ruuvi Station shows a gap. The Bluetooth stack owns the radio while a
connection is up.

Keeping DF5 flowing during a capture — by switching to non-connectable
advertising and back — was tried, and it left the tag permanently silent. The
current design re-asserts one connectable mode from the main loop every slot and
accepts the pause; [ADR-0004](adr/0004-advertising-is-reasserted-every-slot.md)
has the detail and the RTT log that settled it.

Suspending `0xC2` during a capture costs nothing either way: the host has the
same data at 400 Hz.

---

## Compatibility risk

A DF5 parser that assumes *every* `0x0499` payload is DF5 will decode a `0xC2`
chunk as garbage. Correct parsers (Ruuvi Station, Ruuvi Gateway, the Home
Assistant integration) switch on the format byte and drop unknown formats.

If that matters in your deployment, the broadcast spectrum can be disabled and
the tag then looks exactly like a Ruuvi tag with a movement counter stuck at 0.
