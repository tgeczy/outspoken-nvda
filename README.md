# outspoken-nvda

An NVDA synthesizer driver for **MacinTalk**, the speech engine that introduced
the Macintosh in January 1984 — written by Joseph Katz and Mark Barton, who went
on to found SoftVoice.

The engine is real 68000 code. This project runs it under an emulator and models
only the handful of Macintosh services it actually touches.

> **Status: the engine runs.** `DriverOpen` executes to completion under Musashi
> with no stubbed traps and no memory faults. Speech is not wired up yet. See
> [`outspoken-nvda-notes.md`](outspoken-nvda-notes.md) for the working log.

## You must supply the engine

**This repository contains no part of MacinTalk or outSPOKEN, and releases built
from it never will.** Those are © 1984 Katz and Barton and © 1989 Berkeley
Systems (later ALVA BV), and we have no permission to redistribute them.
Emulating code is not a licence to ship it.

The add-on reads the engine out of its `rom/` folder, which arrives empty. On
first launch, if it is empty, you get one dialog offering to open that folder in
Explorer so you can paste in a copy extracted from your own outSPOKEN disk.

## What was learned by reading the binary

All of this came out of a disassembler before anything was executed, and
execution has since confirmed the parts it can reach:

* **[`docs/sound-model.md`](docs/sound-model.md)** — MacinTalk plays through the
  **Sound Manager**, not the DAC: `bufferCmd` + `callBackCmd`, two buffers of
  8-bit unsigned PCM at **22254.5455 Hz**. There is no timing to model, which is
  what makes this tractable at all.
* **[`docs/driver-api.md`](docs/driver-api.md)** — speech is `_Write`, not
  `_Control`. `Control` sets rate (0–4096, default 150), pitch (65–500 Hz) and
  voice. There are **two voices, differing only in pitch — 110 Hz and 250 Hz**.
  Setting `CPUFlag` to 0 keeps the driver away from its only 68020 instructions,
  so a plain 68000 core suffices.
* **[`docs/frame-format.md`](docs/frame-format.md)** — the synthesiser's
  **8-byte frame**: three formant increments, three amplitudes, a voicing
  selector and a gain. Also why the routine installed by
  `SetStopSpeechCallback` is **not a notification** — it reads two bytes of
  every frame, and the synthesiser cannot advance without it.
* **[`docs/cpu-core-decision.md`](docs/cpu-core-decision.md)** — why Unicorn
  cannot run this and Musashi can.

## Tools

None of these need an emulator; they read the binary.

| tool | what it does |
|---|---|
| `tools/symbol_map.py` | recovers all 18 routines from the MacsBug names left in the shipped binary, with exact extents |
| `tools/disasm.py` | capstone m68k with A-traps resolved by name; takes a symbol or an address range |
| `tools/osp.py` | ctypes binding for the host DLL |
| `tools/probe_open.py` | runs `DriverOpen` and reports every trap it asked for |

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
