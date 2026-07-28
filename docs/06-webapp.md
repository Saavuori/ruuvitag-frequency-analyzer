# 06 — The web app

```bash
python run.py --stream AA:BB:CC:DD:EE:FF
```

Then `http://127.0.0.1:8765`. One process holds the radio, the database and the
UI, so there is no way for the three to disagree about what was captured.

## The layout is the idea

The spectrum trace and the waterfall share **one frequency axis and one
horizontal mapping**, stacked, with a single cursor running through both. Find a
peak in the trace, follow it straight down, and see whether it has been there
for four minutes or arrived ten seconds ago.

That is why frequency is on X and time runs downward, rather than the usual
spectrogram convention of time on X. It costs nothing and it makes the two views
one instrument instead of two charts.

```
┌──────────────────────────────────────┐
│ SPECTRUM   live · max hold · recent  │
├──────────────────────────────────────┤
│ HISTORY    newest at top, scrolls ↓  │
│                                      │
└──────────────────────────────────────┘
  1 Hz ────────── frequency ────── 50 Hz
```

## What the controls buy

The control rail shows settings on the left and **their consequences on the
right**: resolution, window length, refresh interval, sample rate. Resolution
and window length are the same fact, so showing them apart would hide the only
real trade-off in the instrument.

| Control | Note |
|---------|------|
| FFT 256–4096 | Longer = finer bins *and* a longer window. Both move together. |
| Overlap | Raises the column rate. Does not improve resolution. |
| Window | Hann by default. Flat top when you care what something measures rather than where it is. |
| Axis | X, Y, Z, or `\|xyz\|` — which **doubles** frequencies, and is labelled for it. |
| Top of scale / Range | The colour map and the trace grid. "Fit to signal" picks them from percentiles of what is actually present. |

## Honesty in the drawing

**Lost windows are drawn as gaps**, in the panel colour, not as the bottom of
the colour scale. The bottom of the scale means silence; a gap means we do not
know. `imageSmoothingEnabled` is off so the canvas cannot interpolate across one.

**A trace below the scale rides the floor** rather than vanishing. That is what
an analyser does when something is below the range you asked for, and it is
visibly different from a break in the line.

**Persistence is real data.** The faint traces behind the live one are the last
24 spectra, not a decay effect. How widely they spread at a frequency is how
much that component is wandering — something a single frame cannot show.

**"None above noise" is a result.** When nothing clears the prominence test the
readout says so rather than naming the tallest noise bin.

## Offline by construction

No CDN, no external fonts, no cloud, no account. The charts are hand-drawn onto
canvas for the same reason. The tag works with no network and the instrument
reading it should too (P-4).

Type is `Bahnschrift` where available — Windows' DIN 1451, the lettering
standard used on machine panels and test equipment — falling back through
condensed grotesques to the system UI face. It is already on the machine, which
is the point.

## API

| Endpoint | Returns |
|----------|---------|
| `GET /api/tags` | Known tags, which are streamable, and stream status |
| `GET /api/status` | Collector statistics |
| `GET /api/spectrogram?mac=&since=&nfft=&overlap=&window=&axis=&flo=&fhi=` | Columns, frequencies, peak, bands, loss counts |
| `GET /api/broadcast?mac=&since=` | Reassembled 0xC2 frames |
| `GET /api/series?mac=&since=` | DF5 readings |
| `POST /api/stream` `{"mac": "..."}` | Start capture; `{"mac": null}` stops |

Columns are `null` where a window contained lost samples. NaN is not JSON; null
is, and null is what the UI draws as a gap.
