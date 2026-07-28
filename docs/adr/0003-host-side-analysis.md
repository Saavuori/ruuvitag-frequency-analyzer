# ADR-0003 — The host does the analysis

**Status:** Accepted.

## Context

The straightforward design computes spectra on the tag: a 256-point transform,
Hann window, 50% overlap, Z axis, all compiled in. Changing any of them then
means rebuilding and reflashing a device that may be screwed to something.

## Decision

The tag transports samples. **Everything downstream of acquisition runs on the
host**, in `webapp/dsp.py`, with transform length, overlap, window function,
axis, band limits and reference level as request parameters.

The firmware keeps exactly one transform, for the connectionless `0xC2`
broadcast, and `firmware/src/fft.h` says in as many words not to grow it.

## Rationale

**Analysis settings are exploratory by nature.** You do not know the right
window length until you have looked at the signal. Making that a reflash makes
it a decision instead of an experiment.

**One implementation, tested.** `webapp/dsp.py` is the code
`tests/test_dsp.py` exercises against tones of known amplitude and frequency.
Doing the transform in JavaScript for the browser would have been a second
implementation and a second thing to be wrong.

**Raw samples are auditable; spectra are not.** A spectrum computed on the tag
can only be checked against itself. Samples can be re-transformed at a different
resolution, compared against a second implementation, and re-analysed later when
you realise you were looking at the wrong band.

**It found a real bug.** Because the host transform is tested against planted
tones, an amplitude normalisation error — reading an on-centre tone 18% low —
was caught in a unit test rather than being believed on a screen. The firmware's
own transform had the same error and was fixed to match.

## Consequences

- The firmware got smaller and has fewer opinions.
- numpy is a hard dependency of the host side. It is not a hard dependency of
  the protocol: `webapp/protocol.py` is pure stdlib, so a minimal consumer can
  decode without it.
- The transform runs server-side, not in the browser, so changing a setting is
  an HTTP round trip. Locally that is milliseconds.
- Long captures re-transform on every request. For the minutes-long windows this
  is built for that is fine; a multi-hour window at fine resolution would want
  caching, and does not have it.
