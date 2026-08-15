# outspoken-nvda

An NVDA synthesizer driver for **MacinTalk**, the speech engine that introduced
the Macintosh in January 1984 — written by Joseph Katz and Mark Barton, who went
on to found SoftVoice.

The engine is real 68000 code. This project runs it under an emulator and models
only the handful of Macintosh services it actually touches.

> **Status: the add-on works.** MacinTalk speaks inside NVDA — English text,
> both voices, rate and pitch, working interruption. The engine takes phonemes
> only, so English is converted by an interpreter written for this project
> reading the user's own `RULZ` rules and `DICT` exception list
> (see [`docs/softvoice-lineage.md`](docs/softvoice-lineage.md) for why the
> front end is a separate component at all). 38 tests:
> `py -3 -m pytest tests -q`. Working log in
> [`outspoken-nvda-notes.md`](outspoken-nvda-notes.md).

## You must supply the engine

**This repository contains no part of MacinTalk or outSPOKEN, and releases built
from it never will.** Those are © 1984 Katz and Barton and © 1989 Berkeley
Systems (later ALVA BV), and we have no permission to redistribute them.
Emulating code is not a licence to ship it.

The add-on reads the engine out of **`outspoken-roms`** in your NVDA
configuration folder, which arrives empty — not the add-on's own directory,
because updating an add-on deletes and recreates that and would take your engine
with it. On first launch you get one dialog offering to open the folder in
Explorer.

`tools/extract_rom.py` fills it from a disk image or an outSPOKEN file you
already have:

```sh
py -3 tools/extract_rom.py "C:/path/to/outSPOKEN"
py -3 tools/extract_rom.py "C:/path/to/MacOS7.hfv"      # needs machfs
```

It classifies what it finds by what is inside it rather than by name, extracts
what this project can use, and says plainly what it skipped and why.

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
