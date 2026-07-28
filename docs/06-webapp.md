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
| Band | 1–150 Hz by default. 1–188 reaches Nyquist and the measurably noisier top end; narrower spans just zoom in. Gridlines and band-level slices follow whatever is selected. |
| Top of scale / Range | Continuous sliders, live while dragging. They set the colour map and the trace grid together. |

"Fit to signal" reads percentiles of what is actually present and sets both.
Sliders rather than presets because the value that makes a signal legible is
continuous: with dropdown steps, autoscale had to snap to whichever preset was
nearest and throw away the answer it had just computed. On real data it now
lands on values like 98 dB that no sensible preset list would have contained.

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

## RPM, and why it is shown three ways

Rotating machinery is specified in RPM, so hovering shows it — but a peak is
only the shaft speed if it is the **1×** component, and plenty of machines put
their largest peak elsewhere. A fan's loudest line is usually blade-pass (shaft
× blade count); misalignment shows at 2×; a belt runs at its own rate entirely.

So the readout gives 1× and the two sub-harmonic readings and lets you decide
which is the shaft:

```
30.79 Hz      1847 RPM at 1×
50.4 dB (0.331 mg)   hold 92.7 dB
if 2× 924    if 3× 616 RPM
```

Printing a single confident RPM would be wrong by an integer factor often enough
to matter. The Capture health panel does the same for the auto-detected peak,
and shows nothing at all when no tone clears the prominence test.

A practical way to resolve the ambiguity: if the real shaft rate is present at
all, you will usually see peaks at both f and 2f. The lowest one that has
harmonics above it is the 1×.

## Offline by construction

No CDN, no external fonts, no cloud, no account. The charts are hand-drawn onto
canvas for the same reason. The tag works with no network and the instrument
reading it should too (P-4).

Type is `Bahnschrift` where available — Windows' DIN 1451, the lettering
standard used on machine panels and test equipment — falling back through
condensed grotesques to the system UI face. It is already on the machine, which
is the point.

## Regenerating the README screenshots

The plot figures are the app's own canvases, exported with `toDataURL` — pixel
exact, not a photograph of a screen. The full-window shot is Chrome rendering
the page offscreen:

```bash
chrome --headless=new --screenshot=docs/images/analyzer-app.png        --window-size=1800,1560 --virtual-time-budget=25000 --hide-scrollbars        http://localhost:8765
```

Headless rather than a desktop capture on purpose: a screen grab picks up
whatever window happens to overlap, which on the first attempt included a
bookmarks bar that had no business in a repository.

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
