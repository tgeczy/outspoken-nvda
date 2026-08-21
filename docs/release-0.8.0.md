# outSPOKEN 0.8.0

**A fourth engine, and the pitch slider finally works.** Thirty-four voices
now, across MacinTalk 1, 2, 3 and Pro.

## You need to extract again

MacinTalk 3 was never extracted by any earlier version of this add-on — the
extractor refused it by name — so **re-run the extractor or you will not see
any of its nineteen voices**:

```
py -3 tools/extract_rom.py "path/to/your/disk image" --nvda
```

`--nvda` writes straight into the folder NVDA reads. Without it you get a
`rom/` folder beside the tools and nothing changes, which is the commonest
way this goes wrong.

Nothing you already had is disturbed, and nothing here ships with the add-on.
The voices are yours, from your own disk image.

## MacinTalk 3

Apple's 1994 engine, running as **real 68k code** rather than a native port —
so it is the engine as it actually behaved, not a reconstruction of it. It
brings nineteen voices, and the strange ones are the point:

| | |
|---|---|
| speaking | Fred, Kathy, Princess, Junior, Ralph, Albert, Whisper, Zarvox, Trinoids |
| singing and novelty | Bells, Cellos, Pipe Organ, Good News, Bad News, Boing, Bubbles, Bahh, Deranged, Hysterical |

Nine of them carry wave data and the engine refuses them without it. Which
nine is not a list kept anywhere — each voice names its own wave resource, so
the extractor and the driver cannot disagree.

## Pitch works, on every engine

`soPitchBase` is a **musical scale**: twelve units to the octave, with 60 at
middle C. It is not hertz. The driver had been feeding it hertz, so a request
for "90" meant a note near 2 kHz — past every engine's ceiling, which is why
two different values produced identical audio and it looked broken rather than
obeyed.

The slider is now an offset from each voice's own pitch, an octave either way,
matching the Tiger and Leopard add-ons so one setting means one thing across
all three.

**Capital pitch change percentage works for the first time.** NVDA marks a
capital with a `PitchCommand`, and this driver had only ever kept
`IndexCommand` from the speech sequence — so that setting did nothing at any
value, and nothing in a log would have shown it, because NVDA does not send a
command a driver has not declared.

**One thing moves underfoot:** MacinTalk 1's slider now reaches an octave at
the top where it used to reach a fifth. The middle is unchanged, so your
default is exactly as it was; only high settings sound higher.

## Speech that runs together properly

NVDA hands over the pieces of a line — text, a link, more text — as separate
strings. Each was being spoken as its own utterance, which gave every fragment
the falling intonation of a finished sentence. They are joined now, and an
index no longer splits one either. `BreakCommand` is honoured as a real pause.

## Faster where it was slow

The two newest engines are also the slowest, measured on the same long text:

| engine | render | audio | realtime |
|---|---|---|---|
| MacinTalk 1 | 219 ms | 21.3 s | 97× |
| MacinTalk 2 | 105 ms | 20.4 s | 194× |
| MacinTalk 3 | 1073 ms | 24.9 s | 23× |
| MacinTalk Pro | 1532 ms | 26.5 s | 17× |

Nothing used to play until all of it existed, so a long post began with a
second of silence. MacinTalk 3 and Pro now hand audio over as they render it:
first sound arrives in about 30 and 93 milliseconds.

Interrupting one also stops it. Abandoning a render used to go on doing 40 to
48 per cent of the work anyway, which is why scrolling a timeline dragged and
why typing could not keep up — every keystroke was queued behind audio nobody
would hear.

**MacinTalk Pro stays the slowest and cannot go much lower.** Its
`SpeakBuffer` analyses the whole text before handing back a single buffer —
about 110 ms of a 340 ms render — and that part is not skippable without
changing how the engine is driven. It is a limitation of an engine written in
1993, when nothing was going to hand it a paragraph of somebody's timeline.

## Fixed

* **MacinTalk Pro was truncating every utterance at 23 seconds.** Long
  paragraphs were cut off mid-word. All four engines now allow about 70.
* MacinTalk 3 was leaving 390 ms of silence on the end of every utterance,
  heard as pauses between chunks and as the previous phrase running into the
  next.
* Switching synthesizer during a long utterance could leave the engine open.

## Notes

* 158 tests. MacinTalk 2 and MacinTalk Pro renders are byte-identical to
  0.7.0's, so nothing that already worked has moved.
* Still to come: a volume setting, and inflection.

## Thanks

To Tomi, who found every one of the audio faults above by listening, and who
would not accept "capital pitch change" being quietly absent.
