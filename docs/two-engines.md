# outSPOKEN speaks through either of two engines

Tomi recorded a demo from real outSPOKEN under Basilisk II and said the voice
was not what this project produces — darker, more like Apple's 1990s engine.
Two things turned out to be true, and only one of them was the one I guessed.

## It prefers a driver it does not ship

`Code 128 'Patch'` — outSPOKEN's screen-reader core — asks for a speech driver
**by name**:

```
+01858  move.l  #'DRVR', -(a7)
+0185E  pea     $18f6(pc)          ; '.SPEECH'
        ...                        ; GetNamedResource
+018CC  lea     $18f6(pc), a1
+018D0  move.l  a1, $12(a0)        ; ioNamePtr
+018E0  _Open
```

Parsing the original `outspoken.bin` — a MacBinary wrapper around a
151,552-byte resource fork, 63 resources — gives **exactly one driver**:

```
DRVR   1030    21272 bytes  '.sp'
```

There is no `.SPEECH` in the file. So outSPOKEN looks for an external engine
first and falls back to the one it carries. `.SPEECH` is Apple's **MacinTalk 2**
(c. 1992), which installed itself as a `.SPEECH` `DRVR` in the System Folder;
MacinTalk 3 and Pro came later as Speech Manager components (`'mtk3'`, `'thng'`)
rather than drivers, so they do not answer this call at all.

That is a real capability and worth knowing. **It is not what produced the
demo.**

## The demo was `.sp` — the fallback

The two Basilisk II disk images are not the same machine, and the one the demo
came from is the one *without* Apple's engine:

| | `Starterdisk.hfv` | `O s 7 src.hfv` |
|---|---|---|
| `JOSEPH KATZ` / `MARK BARTON` 1984 | **8 hits** (two copies) | zero |
| `MacinTalk 2` / `3` / `Pro` | **zero** | 69 / 77 / 81 |
| `Speech Manager` | **zero** | 95 |
| `.SPEECH` | 5, all inside outSPOKEN's own code | 10 |

Starterdisk's `.SPEECH` hits sit beside outSPOKEN's own MacsBug routine names
`TURNSPEECHON` and `LOADSPEECH` — they are the caller, not a driver. With no
`.SPEECH` present, the lookup fails and outSPOKEN uses `.sp`.

So the voice on the recording **is** the engine this project emulates, and any
difference is in our chain rather than in a missing engine.

Measured on the capture in `the Basilisk II capture`
(18.9 s, stereo, 48 kHz):

* **f0 median 117.9 Hz** (10th percentile 84.8, 90th 279.1) against 111.8 Hz for
  our male voice at the driver's default pitch of 110. Same engine, same pitch.
* Energy above 11.5 kHz is 52 dB below peak, consistent with a 22 kHz source
  upsampled to 48 — the rate `MACSTARTSOUND` writes into the `SoundHeader`.

## Correction: the demo was a different *product*

Everything above about which disk and which driver was chasing the wrong thing.
The recording says "132 items comma, one comma zero twenty three period nine
mb" -- a Finder window on a **1,023.9 MB** volume. `O s 7 src.hfv` is
1,073,741,824 bytes, exactly 1024 MB; `Starterdisk.hfv` is 9 MB. So the capture
came from the OS 7 disk after all.

Dumping that image (1,223 files, 109.9 MB, via `machfs`) shows why none of the
`.SPEECH` reasoning applied:

```
System Folder/Control Panels/outSPOKEN 8     cdev oSM   rsrc 255187
System Folder/Extensions/MacinTalk 3         thng mtk3  rsrc 358659
System Folder/Extensions/MacinTalk Pro       thng gala  rsrc 572928 + 235892
System Folder/Extensions/Speech Manager      INIT ttsc
System Folder/Extensions/Voices/Fred         ttvf mtk3  rsrc 1157
System Folder/Extensions/Voices/Bruce        ttvf gala  rsrc 801815
```

That is **outSPOKEN 8**, creator `oSM`. The file this project reverse-engineers
is `outspoken.bin`, creator `BSDo` -- an earlier and different product.
outSPOKEN 8's resources contain `ttsc`, the Gestalt selector for the Speech
Manager, and no `.SPEECH`, no `MacinTalk` and no Katz/Barton strings at all. It
also announces "outSPOKEN requires a 68020 or later", where `.sp` deliberately
runs on a plain 68000 with `CPUFlag` at zero.

So the demo is outSPOKEN 8 driving the Speech Manager, and the voice is one of
Apple's. Nothing in `DRVR 1030` will ever sound like it, and that is not a
defect in this emulation.

| | outSPOKEN (`BSDo`) | outSPOKEN 8 (`oSM`) |
|---|---|---|
| engine | bundled `.sp`, 1984 Katz/Barton | Speech Manager (`ttsc`) |
| voices | one male/female pitch pair | Fred, Bruce, Victoria, Agnes, … |
| CPU | 68000 | 68020 or later |

### Those voices are two engines, and one of them is already solved

* **MacinTalk 3 (`mtk3`) voices are tiny** -- Fred, Kathy, Princess, Ralph,
  Junior, Whisper, Zarvox and Trinoids are about 1,160 bytes each. They are
  parameter sets for the 358 KB `MacinTalk 3` component, a formant synthesiser.
* **MacinTalk Pro (`gala`) voices are huge** -- Bruce 801 KB, Agnes 870 KB,
  Victoria 935 KB. A concatenative engine: a different synthesis technique,
  but *not* a different architecture -- see
  [`macintalk2-components.md`](macintalk2-components.md), it is a `ttsc`
  component with the same entry convention as MacinTalk 2.

A native port already builds the formant one from Apple source
(`MT4.h`, `formantSynth.c`, `Wavinout.c`'s `voiceNames[]`), and its
NVDA add-on already lists all seventeen: Fred, Kathy, Princess, Junior,
Ralph, Whisper, Zarvox, Trinoids, Bubbles, Boing, Bells, Hysterical, Deranged,
Good News, Bad News, Pipe Organ, Cellos.

**So Fred is not missing from anywhere. It is a different project, and it is
already built.** The MacinTalk Pro voices -- Bruce, Victoria, Agnes -- are the
only ones nothing covers today, and they are this project's eventual target
because they share MacinTalk 2's component architecture.

## The open difference: we are too bright

Long-term average spectra, each normalised to its own peak:

| band | real | ours | difference |
|---|---|---|---|
| 500–1000 Hz | −17.4 | −6.6 | **+10.8 dB** |
| 1000–1500 | −23.1 | −15.1 | +8.0 |
| 1500–2000 | −29.6 | −17.4 | **+12.2 dB** |
| 2000–2500 | −31.9 | −19.3 | **+12.5 dB** |
| 2500–3000 | −30.2 | −20.6 | +9.6 |
| 3000–3500 | −35.4 | −27.6 | +7.8 |

Our output carries roughly 8–12 dB more energy across the whole formant region.
The real thing is darker; ours is brighter and buzzier, which matches the
listening impression exactly.

**This is suggestive, not conclusive, and after the filter test it is weaker
still.** The capture is 18.9 s of unknown material -- only 20.6% of it above 8%
of peak, so mostly silence and probably interface chatter -- against 4.9 s of a
sentence chosen at random. A long-term average spectrum is sensitive to phoneme
content, and a mid-band dip is exactly what different content looks like.

**Superseded.** The capture is outSPOKEN 8 through Apple's Speech Manager, so
it is not this engine at all and the whole comparison was between two different
synthesisers. That fully explains a mid-band dip no filter could reproduce. The
table is kept only as a record of a measurement that pointed the right way for
the wrong reason -- our output was never supposed to match it.

Candidates, in the order worth testing:

1. ~~**Output filtering.**~~ **Tested and rejected.** Classic Mac hardware
   low-passed its 8-bit output, so a missing filter was the obvious first
   suspect. Fitting a one-pole low-pass to our output against the capture makes
   the match monotonically *worse* -- 4.48 dB RMS unfiltered, 5.62 at 4 kHz,
   8.42 at 2 kHz, 13.90 at 1 kHz. The real capture is not rolled off; it has a
   *dip* through 500-3500 Hz while matching us again at 4500-5000, and no
   low-pass produces a mid-band dip. Do not re-test this.
2. **Front-end differences.** outSPOKEN does not feed the engine plain
   dictionary phonemes — `STR# 128` shows Berkeley respelling everything by
   hand (`brohvo`, `keelo`, `ho tel`, `see ehrra`), and it ships `PHNM`,
   `DICT 'word dictionary'` and `DICT 'graphic dictionary'` besides `RULZ`.
   Different phoneme strings sound different without any engine difference.
3. **Rate and pitch** as the control panel sets them, rather than the driver's
   power-on defaults.

## Two lines, not one lineage

* **`.sp` — Joseph Katz and Mark Barton, 1984.** Four copyright strings say so.
  The SoftVoice line, 67.8% byte-identical to the Amiga `narrator.device`
  (see [`softvoice-lineage.md`](softvoice-lineage.md)). What this project runs.
* **`.SPEECH` — Apple, Tim Schaaff, 1992–94.** MacinTalk 2, 3 and Pro. A
  separate line sharing no code with the above.

Sibling branches, not ancestor and descendant. MacinTalk 3 is already solved
elsewhere: Apple's own MacinTalk source has been ported natively to Windows and
ships as a working NVDA add-on with x64 and x86 DLLs. If both are wanted in NVDA
they are two synthesisers, not two voices of one.
