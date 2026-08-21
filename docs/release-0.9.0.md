# outSPOKEN 0.9.0

**Volume and inflection, on every engine that can do them.** Two settings NVDA
expects from a synthesizer and this one never had, plus the two speech
commands that go with them.

Nothing needs re-extracting. If 0.8.0 works for you, this is a drop-in.

## Volume

Works on all four engines and all thirty-four voices, including MacinTalk
(1984), whose driver has four settings and none of them is amplitude.

It is applied by the driver rather than by an engine, and the reason is
arithmetic. All four produce **eight-bit** audio, so turning it down inside one
would quantise 256 levels into whatever the slider left of them — volume 25
would be six bits and audibly grainy. Folded into the widening to 16-bit
instead, quarter volume is still the full detail of the original.

**There is no boost**, and that was measured rather than assumed. Eleven of
MacinTalk 3's nineteen voices and two of Pro's three already peak at 127 or 128
of a possible 128: there is no headroom to raise anybody into. At 100 this
release renders byte-for-byte what 0.8.0 did.

## Inflection

How far the voice's pitch is allowed to move. The middle of the slider is the
voice exactly as Apple recorded it, and **every voice has its own natural
amount** — Fred and Ralph use four times as much as Albert — so the slider
scales each voice's own depth instead of imposing a number on all of them.

The engines differ more than they agree, and the readme says so:

| engine | inflection |
|---|---|
| MacinTalk (1984) | not available — the driver has no such control |
| MacinTalk 2 | **two states, not a scale**: flat, or the voice as it was |
| MacinTalk 3 | continuous, nineteen voices with five different natural depths |
| MacinTalk Pro | continuous, but it will not go completely flat |

Four MacinTalk 3 voices ignore it — Bad News, Bells, Good News and Hysterical
take their pitch from a tune rather than from a contour. And the voices that
are flat by design — RoboVox, Xero, Zarvox, Trinoids and the rest — **gain**
intonation above the middle of the slider instead of losing it below, since
there was nothing there to take away.

## Speech commands

`VolumeCommand` and `RateCommand` are now honoured inside a speech sequence,
alongside the `PitchCommand` and `BreakCommand` that arrived in 0.8.0. A
command NVDA is not told about is never sent at all, which is what made
"capital pitch change percentage" look broken rather than unsupported.

## A hang, found and fenced off

MacinTalk Pro loops forever if it is asked for a pitch modulation near zero: a
render costing 4.3 million instructions was still going after three billion,
with no fault, no trap and no audio. It needs more than one clause to happen,
so a first probe on a single sentence reported the selector harmless.

**The threshold belongs to the voice rather than to the engine** — Bruce hangs
all the way up to 0.05, Agnes to 0.025, and Victoria only at exactly zero.
Agnes is the first voice and also the default, so a floor fitted to her would
have shipped a synthesizer that froze on the second voice anybody tried. The
bottom of the slider now stops ten times clear of the worst of them, which is
close enough to flat that it cannot be heard.

## Under the hood

* 188 tests, up from 158. The new ones include a byte-identity check that
  volume 100 is exactly the conversion 0.8.0 used, and a per-voice sweep of
  both ends of the inflection slider — the check that would have caught the
  hang.
* `tools/probe_inflection.py` is new and holds the measurements: what each
  voice's own depth is, which engines quantise, and `--cliff` for finding
  where an engine stops coming back.
* The debug log line now carries volume and inflection.
