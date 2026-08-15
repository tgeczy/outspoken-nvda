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

Measured on the capture in `C:\git\outspoken-rsrc\outspoken_sample.wav`
(18.9 s, stereo, 48 kHz):

* **f0 median 117.9 Hz** (10th percentile 84.8, 90th 279.1) against 111.8 Hz for
  our male voice at the driver's default pitch of 110. Same engine, same pitch.
* Energy above 11.5 kHz is 52 dB below peak, consistent with a 22 kHz source
  upsampled to 48 — the rate `MACSTARTSOUND` writes into the `SoundHeader`.

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

**The one measurement that would settle it: render the words the capture
actually speaks.** Until then this table is a lead, not a finding.

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

Sibling branches, not ancestor and descendant. The Apple branch is already
solved elsewhere: `C:\git\wintalker` holds `MacInTalkSrc.zip` and a working
`NVDA_addon/` with x64 and x86 `WinTalker.dll` built from it. If both voices are
wanted in NVDA they are two synthesisers, not two voices of one.
