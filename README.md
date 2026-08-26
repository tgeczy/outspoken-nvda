# outspoken-nvda

An NVDA synthesizer driver for three Macintosh speech engines, all run as real
68000 code under an emulator:

* **MacinTalk (1984)** — the engine that introduced the Macintosh, written by
  Joseph Katz and Mark Barton, who went on to found SoftVoice. Two voices.
* **MacinTalk 2 (Apple, 1992)** — ten more: Ben, Boris, Brenda, Mariel, Marvin,
  Mr. Hughes, Otis, RoboVox, Votron and Xero.
* **MacinTalk Pro (Apple, 1993)** — Agnes, Bruce and Victoria. The first
  concatenative Macintosh voices, and the ones most people mean when they
  remember what a Mac sounded like. Pro needs a 68020, is addressed by resource
  *name* rather than id, and reads its 573 KB lexicon out of its own file as it
  speaks — asynchronously, which is why it needed a File Manager that answers
  completion routines.

**As far as we can establish, neither MacinTalk 2's voices nor MacinTalk Pro's
have run outside a Macintosh before.**

This project models only the handful of Macintosh services the engines actually
touch. Whichever of them you have extracted are the ones offered, so any one
alone is enough.

**What ships here is a machine with no tape in it.** The outSPOKEN engine and
its voices are Berkeley Systems' work, and their rights did not evaporate with
the software: they passed through ALVA to Vispero, who make the screen reader
half this community runs today. So no release of this project contains a byte
of engine data — not the add-on, not the SAPI installer — and the packaging
script refuses to build one that does. What you get instead are the tools:
point them at your own disk, `.bin` or disk image, and the extractor stages
the engines where both drivers find them. If a download claims to be these
voices ready-to-run, it is somebody redistributing property that is not theirs
to share, and it did not come from here.

## The SAPI 5 driver

Beside the NVDA add-on there is a SAPI 5 engine, for JAWS, System Access,
and anything else that speaks SAPI — and it is deliberately not a port.
The engine DLL launches an embedded Python running `sapi/osp_serve.py`,
which serves **the same driver modules NVDA loads**, so the SAPI voice is
byte-identical to the NVDA voice by construction;
`tests/test_sapi_serve.py` asserts exactly that, byte for byte. Measured on
this machine under the interpreter the installer ships: 21 ms from request
to first sound with the host warm, 158 ms cold. The fragment handling carries the JAWS lessons learned in
TGSpeechbox and Panthera: word-per-fragment feeding with bookmarks between
never reads bookmark names aloud, and the seam between fragments keeps its
space.

> **Status: all three engines speak inside NVDA.** English text, fifteen
> voices, rate, interruption, symbols and numbers. 111 tests:
> `py -3 -m pytest tests -q`. Working log in
> [`outspoken-nvda-notes.md`](outspoken-nvda-notes.md).
>
> MacinTalk Pro's Agnes, Bruce and Victoria arrived on 2026-08-20. Two host
> services stood between "it opens" and "it speaks", and neither was in the
> engine: Pro reads its lexicon with an **asynchronous** `_Read` and parks the
> module that asked until the completion routine wakes it, and `_FixRatio` was
> never served at all — which cost twenty million out-of-range reads in a
> single utterance. Both are the same mistake: **a trap answered in the wrong
> mode is a lie the caller cannot detect.**
>
> MacinTalk 1 takes phonemes only, so English is converted by an interpreter
> written for this project, reading the user's own `RULZ` rules and `DICT`
> exception list (see
> [`docs/softvoice-lineage.md`](docs/softvoice-lineage.md) for why the front
> end is a separate component at all). MacinTalk 2 ships its own front end and
> is driven through the Component Manager, which the host implements —
> see [`docs/macintalk2-components.md`](docs/macintalk2-components.md).
>
> **Numbers are read as words**, which neither engine can do: their rules hold
> the ten digit names and nothing else, so `30` would otherwise be "three
> zero". It is a checkbox, because digit-by-digit is genuinely better for
> phone numbers and identifiers.
>
> Not yet done: a user pronunciation dictionary. (The pitch sliders were
> once listed here as deliberately inert; MacinTalk 2, 3 and Pro all
> behave now — a probe-calibrated slider, twelve semitones either side of
> each voice's own pitch.)

## You must supply the engine

**This repository contains no part of MacinTalk or outSPOKEN, and releases built
from it never will.** Those are © 1984 Katz and Barton, © 1989 Berkeley Systems
(later ALVA BV) and, for MacinTalk 2, © Apple, and we have no permission to
redistribute any of them. Emulating code is not a licence to ship it.
`tools/package.py` refuses to build a release if anything resembling engine
data is in the tree.

The add-on reads the engine out of **`outspoken-roms`** in your NVDA
configuration folder, which arrives empty — not the add-on's own directory,
because updating an add-on deletes and recreates that and would take your engine
with it. On first launch you get one dialog offering to open the folder in
Explorer.

`tools/extract_rom.py` fills it from a disk image or an outSPOKEN file you
already have:

```sh
py -3 tools/extract_rom.py "C:/path/to/outSPOKEN" --nvda
py -3 tools/extract_rom.py "C:/path/to/MacOS7.hfv" --nvda   # needs machfs
```

`--nvda` writes straight into the folder the add-on actually reads. Without it
the files land in `./rom`, which is fine for the tools in this repository and
invisible to NVDA — a distinction worth one flag, because "extracted it and
nothing changed" is the commonest way this goes wrong.

It classifies what it finds by what is inside it rather than by name, extracts
what this project can use, and says plainly what it skipped and why. It then
reports **what NVDA will actually offer**, using the same function the add-on
uses, and names anything it will not:

```
  NVDA will offer, from C:\Users\you\AppData\Roaming\nvda\outspoken-roms:
    MacinTalk 2    10 voices  Ben, Boris, Brenda, Mariel, ...
    MacinTalk Pro   3 voices  Agnes, Bruce, Victoria

  Present but NOT offered, and why:
    Fred and 19 more    MacinTalk 3 is not installed
    Agnes               incomplete extraction, missing rsrcfork.bin
```

**If you extracted before 0.7.0, re-run it.** MacinTalk Pro's voices need far
more than earlier versions took — the unit database, the per-voice code and the
resource fork itself — so an older `voices/Agnes` holds the descriptor and
nothing else. Those are not offered rather than offered-and-silent, and re-
running completes them in place without losing anything.

## What was learned by reading the binary

All of this came out of a disassembler before anything was executed, and
execution has since confirmed the parts it can reach:

* **[`docs/sound-model.md`](docs/sound-model.md)** — MacinTalk plays through the
  **Sound Manager**, not the DAC: `bufferCmd` + `callBackCmd`, two buffers of
  8-bit unsigned PCM at **22254.5455 Hz**. There is no timing to model, which is
  what makes this tractable at all.
* **[`docs/driver-api.md`](docs/driver-api.md)** — speech is `_Write`, not
  `_Control`. `Control` sets rate (0–4096, default 150), pitch (65–500 Hz) and
  voice. **Two voices, male at 110 Hz and female at 250 Hz** -- that is the
  whole list. A formant-table flag exists but the alternate table renders a
  2.23 s sentence in 0.18 s, so it is broken rather than robotic.
  Setting `CPUFlag` to 0 keeps the driver away from its only 68020 instructions,
  so a plain 68000 core suffices.
* **[`docs/frame-format.md`](docs/frame-format.md)** — the synthesiser's
  **8-byte frame**: three formant increments, three amplitudes, a voicing
  selector and a gain. Also why the routine installed by
  `SetStopSpeechCallback` is **not a notification** — it reads two bytes of
  every frame, and the synthesiser cannot advance without it.
* **[`docs/softvoice-lineage.md`](docs/softvoice-lineage.md)** — MacinTalk and
  the Amiga `narrator.device` are **67.8% byte-identical**, with the same
  111-entry phoneme table and a letter-to-sound rule set 60.2% shared with
  `translator.library`. Which settles the architecture: the driver speaks
  phonemes only, and the English front end is a separate component.
* **[`docs/cpu-core-decision.md`](docs/cpu-core-decision.md)** — why Unicorn
  cannot run this and Musashi can.

## Tools

| tool | what it does |
|---|---|
| `tools/symbol_map.py` | recovers all 18 routines from the MacsBug names left in the shipped binary, with exact extents — no emulator needed |
| `tools/disasm.py` | capstone m68k with A-traps resolved by name; takes a symbol or an address range — no emulator needed |
| `tools/osp.py` | ctypes binding for the host DLL |
| `tools/probe_open.py` | runs `DriverOpen` and reports every trap it asked for |
| `tools/probe_speak.py` | the full sequence, and writes `build/spoken.wav`. Takes **phonemes**: `py -3 tools/probe_speak.py "AY1 KAEN SPIY1K AXGEH1N"` |
| `tools/probe_frames.py` | measures the frame stride and dumps the block before playback mutates it |

The host carries four instruments, and every one of them settled a question the
disassembly had answered wrongly: a write watchpoint, a read watchpoint (a ring,
because generation does thousands of reads before playback starts), a PC trace
ring, and `osp_snap_set`/`osp_snap_halt`, which record `d0-d7`/`a0-a7` at a
chosen PC and stop there. Reach for them early; see
[`docs/frame-format.md`](docs/frame-format.md) for what it cost not to.

## Building

Needs MSVC build tools, CMake-free, and `git` on PATH. Musashi is fetched at
build time rather than vendored.

```sh
sh build.sh          # x64 and x86
sh build.sh x64      # just one
py -3 tools/probe_open.py
```

The DLL links the **static** CRT deliberately: a `/MD` build needs a Visual C++
redistributable that NVDA does not ship, which is invisible on a development
machine and fatal on a clean one.

## Credit where it is due

* **Musashi** — Karl Stenerud's 68000 core, the one MAME uses. MIT.
* **Huge thanks to Jayson Smith ([@jaybird110127](https://github.com/jaybird110127))
  and his work on EchoTalk**, which is the pattern this project follows: vendor a
  small CPU core, model only what the engine touches, and let the user supply the
  engine. His warning about giving the CPU enough time to finish is designed into
  this host from the first commit — every budget here has a counter, and a
  non-zero count is a logged fault rather than a silent truncation.
* **Joseph Katz and Mark Barton** — who wrote MacinTalk, and whose next company
  is baked into its own pronunciation table: `[SOFTVOICE]=SAA4FTVOYS`.

## Licence

MIT, for our code. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
