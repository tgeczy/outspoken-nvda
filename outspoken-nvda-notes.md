# outSPOKEN / MacinTalk 1984 — working notes

**Goal:** an NVDA add-on that speaks with the original 1984 MacinTalk, by
running the real 68000 code under an emulator. Same architecture as
`pctalker-nvda` and Jayson Smith's EchoTalk: run the actual binary, model only
what it touches, ship the emulator freely and let the user supply the engine.

Status: **the engine runs.** Musashi is vendored and building, the host DLL
exists for x86 and x64, and **MacinTalk's `DriverOpen` executes to completion
and initialises itself exactly as the static analysis predicted** — see
"First execution" below. The sound model is settled (`docs/sound-model.md`) and
the driver API is mapped (`docs/driver-api.md`).

## First execution — 2026-08-15

`py -3 tools/probe_open.py`:

```
  +0006C  _NewHandle                 -> 0x00080B00     ($B00 = 2816, as predicted)
  +0008E  _CmpString                 STUBBED
  +0012A  _HLock
  +00162  _HUnlock
  stop: returned to sentinel, D0 = 0        44 instructions, 0 faults

  voice  $3A  = 0        rate  $32 = 150     pitch $30 = 110
  v0 rate $C6 = 150      v0 pitch $C4 = 110
  v1 rate $CC = 150      v1 pitch $CA = 250
```

Every one of those values was written into `docs/driver-api.md` from the
disassembly *before* anything ran. **Execution confirmed the reading.**

### The bug that made it "work" in 14 instructions

The first run returned cleanly having done nothing. An OS trap returns its
error in `D0` **and sets the condition codes from it** — callers branch on the
flags. Our handler set `D0` and left the flags alone, so `_NewHandle`'s
following `bne` saw a stale `Z` and took the error exit, which returns
`noErr`. A clean success that allocated nothing.

The fix is in `service_atrap`: patch the **stacked** SR, not the live one, since
the `rte` restores from the stack. Toolbox traps (`$A800+`) return on the stack
and are left alone.

Worth keeping as a shape: *a plausible success is the expensive failure*. The
only reason it was caught in a minute is that the doc said Open should reach
`_GetResource` and the log showed it did not.

---

## The find

`D:\B II\outspoken.bin` — outSPOKEN by Berkeley Systems, a Mac control panel
(`'cdev'`, creator `'BSDo'`), 151,680 bytes, MacBinary, **© 1988 Berkeley
System Design**. All 63 resources extracted to `C:\git\outspoken-rsrc\`, with
`FINDINGS.md` alongside.

**`'DRVR' 1030`, the driver named `.sp`, 21,272 bytes, IS the original
MacinTalk.** Four copyright strings inside it:

```
+00420  COPYRIGHT 1984, JOSEPH KATZ / MARK BARTON
+005D1  COPYRIGHT 1984, JOSEPH KATZ / MARK BARTON
+02966  COPYRIGHT 1984 MARK BARTON & JOSEPH KATZ
+03676  COPYRIGHT 1984 MARK BARTON & JOSEPH KATZ
```

Katz and Barton wrote the MacinTalk that introduced the Mac in January 1984 and
later founded **SoftVoice**. This is that line — **not** MacinTalk 2/3, which
is Tim Schaaff at Apple, 1992–94, and whose source is in `C:\git\wintalker`.
Those are sibling branches, not ancestor and descendant.

### Driver entry points

| routine | offset |
|---|---|
| Open | `+0x005A` |
| Prime | `+0x4BA4` |
| Control | `+0x018E` |
| Status | `+0x02E0` |
| Close | `+0x028A` |

**Speech goes through `Prime` (`_Write`), not `Control`** — an earlier note here
said the opposite and was wrong. `Control` sets voice, rate and pitch; there are
two voices, differing only in pitch (110 Hz and 250 Hz). There is also an
**export table at +$0014** that Berkeley's own code jumps through. Full map with
ranges, defaults and the `CPUFlag` requirement: **`docs/driver-api.md`**.

### Routine map — run `py -3 tools/symbol_map.py`

MacsBug names survived in the shipped binary, and each one sits *after* the
routine it names. That gives exact extents for free, which matters because a
disassembler will happily decode the phoneme tables as instructions:

```
0x0014 SetStopSpeechCallback   0x005A DriverOpen  <-- driver Open
0x0182 ...the synthesiser, 19 KB, unnamed...      <-- Control/Close/Status/Prime
0x4CA0 ResetBufLength   0x4CE6 SetupA3
0x4E84 OpenSound        0x4E98 CloseSound      (8-byte empty stubs)
0x4EAE ChannelBusy      0x4EF4 WaitSoundDone   0x4F40 SilenceChannel
0x4F9C STOPSPEECH       0x4FCC EndOfSpeech     0x50C4 CALLBACK
0x5100 MACSTARTSOUND    0x51AA CtrlDown        0x51D0 MACSTOPSOUND
0x524A ClearBuffers     0x5282 StuffA3
```

The four remaining entry points (`Control` +0x018E, `Close` +0x028A, `Status`
+0x02E0, `Prime` +0x4BA4) all fall in the big unnamed block — that block is the
1984 synthesiser itself and carries no symbols.

`Reader` at +0x187 is *not* a symbol: it is inside a Pascal string constant that
`DriverOpen` feeds to `_CmpString`. Cosmetic, but it is why the scanner does not
list it.

### ANSWERED — `.sp` is not self-contained, and that is good news

It uses the **Sound Manager**: `bufferCmd` (81) + `callBackCmd` (13) through
`_SndDoCommand`, two buffers of **3870 8-bit unsigned samples at 22254.5455 Hz**.
There is no `.Sound` string in the binary and no DAC pacing to model. Full
evidence and the complete list of traps the host must service:
**`docs/sound-model.md`**.

It also pulls outSPOKEN's own resources — `DriverOpen` calls
`GetResource('RULZ', 130)`, `GetResource('RULZ', 129)` and
`GetResource('TALK', 1)`. Berkeley wrote the text front end (`RULZ` rules,
`PHNM` phonemes, two `DICT`s); MacinTalk is the phoneme back end. Note the IDs
are offset by 1000 from what the cdev actually stores (`RULZ 1129`, `TALK
1001`), and we have only one `RULZ`.

---

## CPU core: Musashi, not Unicorn

**Unicorn cannot run this.** Its m68k target comes from QEMU, which grew up
around ColdFire, and ColdFire deleted the instructions real 68000 code lives on.
Measured:

| instruction | result |
|---|---|
| `movem.l a0-a2,-(a7)` | **illegal** |
| `movem.l (a7)+,a0-a2` | **illegal** |
| `movem.l (a0),d0-d1` | ok — the form ColdFire kept |
| `dbra d0,-4` | **illegal** |
| `link`/`unlk`/`bsr.w`/`rts`/`lea (pc)`/`swap`/`ext.l` | ok |

MacinTalk's `Open` dies on its second instruction. `rts` looked broken too and
was not — that was a bad test harness returning into unmapped zeros. Check
before concluding.

**Use Musashi** (Karl Stenerud's 68000 core, the one MAME ships — plain C, MIT,
built for embedding behind memory callbacks). Wrap it as a DLL and drive it from
Python with `ctypes`, exactly as EchoTalk does with `fake6502`.
`C:\git\wintalker` already proves CMake + MSVC building x86 and x64 DLLs on this
machine.

### Two things Unicorn taught us that carry over to any core

* **A-line traps**: a Toolbox call surfaces as an exception with PC parked on
  the `$Axxx` word. Read it, service it, advance PC by 2.
* **`illegal` loops forever** — PC does not advance, so it re-fires. Terminate
  on a sentinel address, the way `pctalker_speaker.py` does.

---

## The recording is the WRONG engine — my error, recorded so it is not repeated

`C:\git\outspoken-rsrc\outspoken_sample.wav` — 48 kHz, 16-bit stereo, 18.9 s.
Whisper: *"Welcome to outSPOKEN, Anne"*, then punctuation read aloud
(`comma`, `period`) as a screen reader does. F0 about 123 Hz with a clean
harmonic stack (246, 372, 495, 615, 735…), 90% of energy below 5.5 kHz.

**But the disk has outSPOKEN 8.0, ALVA BV, © 1997–98**, which requires a 68020
and speaks through the **Speech Manager** — `ttsc`, `[[pbas +7]]`, `[[rate]]`.
That is PlainTalk, i.e. **MacinTalk 3, the `wintalker` branch**. So this
recording is the sibling engine, not `.sp`.

Lesson: check *which version is installed* before capturing a reference.

Real 1984 reference audio needs the 1988 cdev running on an older System —
a separate expedition, not a blocker.

### "Anne"

The 1988 code holds `Welcome to outSPOKEN,` as a 21-character Pascal string
ending in the comma; the name is appended at runtime. Nearby, `BLURBDIALOG`:

```
Please enter your name.
3PLIY5Z EH1NTER YOHR NEY2M, DHEH1N /HIH1T RIY1TER1N.
```

Tomi did not type it. **Anne is a previous owner**, her name still in the disk
image, still spoken aloud on launch.

---

## Test vectors — better than the recording

The 1988 resources carry announcements **pre-written in MacinTalk phonemes with
stress digits**, which bypasses the text front end entirely and lets the
synthesizer be tested on its own:

```
3PLIY5Z EH1NTER YOHR NEY2M, DHEH1N /HIH1T RIY1TER1N.
    = "Please enter your name, then hit return."

1DHIHS KAA1PIY IHZ FOHR DIH2MUNSTREY3SHUN OW3NLIY.
    = "This copy is for demonstration only."
```

The phoneme alphabet also appears in the driver at `+0x015AE`:
`IYIHEHAEAAAHAOUHAXIXERUXQXOHRXLXEY`, then `NXNHDXQ`, `ULUMUNILIMIN`.

---

## Files

| path | what |
|---|---|
| `D:\B II\outspoken.bin` | the 1988 control panel, MacBinary |
| `C:\git\outspoken-rsrc\` | all 63 resources, one file each |
| `C:\git\outspoken-rsrc\FINDINGS.md` | the MacinTalk identification |
| `C:\git\outspoken-rsrc\outspoken_sample.wav` | the (wrong-engine) capture |
| `C:\git\outspoken-nvda\tools\trap_probe.py` | loads `.sp`, calls Open, logs traps |
| `C:\git\outspoken-nvda\docs\cpu-core-decision.md` | why Unicorn is out |
| `C:\git\wintalker\` | MacinTalk 2/3 source, sibling branch, builds x86+x64 |
| `D:\B II\BasiliskII_prefs` | repaired (paths were `C:`, files are on `D:`); backup `.bak-2026-08-15` |

Basilisk fix, for reference: `rom` path corrected, missing `Mac OS 8.1.hfv`
dropped, `Starterdisk.hfv` added, **`noaudio true` → `false`** (it was muted),
dead `typemapfile` cleared.

---

## Next steps

1. ~~Find where audio is written.~~ **Done** — `docs/sound-model.md`.
2. ~~Disassemble `Control`.~~ **Done** — `docs/driver-api.md`. The `RULZ`
   resource worry is closed too: that probe is unreachable dead code.
3. **Vendor Musashi**, build the DLL for x86 and x64 alongside the wintalker
   toolchain (`C:\git\wintalker` already proves CMake + MSVC here). A plain
   **68000** core is enough — set `CPUFlag` (`$012F`) to 0 and the driver's only
   68020 instructions (`movec`) are never reached.
4. **Port `trap_probe.py` to Musashi** and run `Open`. It should want
   `_NewHandle($B00)`, then `_GetResource('TALK', 1)` — serve it the extracted
   `TALK 1001` with the +1000 ID mapping.
5. Hand over a channel and two 3892-byte buffers via the export table at
   `+$001E`, `PBWrite` the text, harvest PCM at `bufferCmd`, resample
   22254.5455 Hz → device rate, wrap for NVDA.

### Still unknown

* Who calls `StuffA3` (+$5282) and the buffer-swap wait (+$52BC). No branch in
  the image targets either, so the synthesiser reaches them through a pointer —
  likely one of the globals at `0x4C16`. Not blocking; it will show up the first
  time the engine runs.
* The driver's globals sit **inside the resource image** at `0x4C16`, reached
  PC-relative from every routine. So the 21,272 bytes are **not read-only** —
  map them writable, and reload a fresh copy per utterance if runs ever need to
  be repeatable. This is the kind of thing that surfaces as "the second
  utterance sounds wrong".

### Tools that exist now

| tool | what it does |
|---|---|
| `tools/symbol_map.py` | recovers all 18 MacsBug routines and their extents |
| `tools/disasm.py` | capstone m68k with A-traps resolved by name; takes a symbol or a range |
| `tools/trap_probe.py` | the Unicorn probe — kept for reference, Unicorn cannot run this code |

### Design in from commit one, both learned the hard way

* **Every budget gets a counter, and non-zero is a logged fault.** Jayson's
  warning to Tomi was exactly this: his first fix failed because the 6502 was
  not given enough time to run. A limit that truncates silently gets got wrong
  again. See `notes/step_budget_truncation.md` in `C:\git\echotalk`.
* **The user supplies `outspoken.bin`.** Empty `rom/` directory in the add-on,
  README instructions, a `--with-images` flag for personal builds only.
  Retrofitting this is how `READSPF.EXE` ended up tracked in pctalker.

  **Tomi's UX, decided 2026-08-15 — no settings panel for this.** On first
  launch, if `rom/` is empty, one dialog:

  > "ROM for outSPOKEN is not present. Press OK to open the empty ROM folder in
  > Windows Explorer, where you can paste a working outSPOKEN ROM."

  OK runs `os.startfile(rom_dir)`. That is the whole import flow. Implement it
  in a `globalPlugin` so it can appear at startup, and have `SynthDriver.check()`
  return whether the ROM exists so the synth simply is not offered until it does.

  **Distribution, per Tomi:** the Hungarian synths (PC-ROBOT, BraiLab, PC-TALKER,
  FlexVoice) may ship with binaries bundled *in the release*, never in the repo,
  because we hold distribution rights from their authors. **outSPOKEN has no such
  permission** — GitHub gets code only; a bundled build stays on Tomi's own
  Eurpod synths folder for friends. See [[kiraly-correspondence-state]].

---

## Rights

outSPOKEN is Berkeley Systems, later ALVA BV. MacinTalk is Katz and Barton.
There is no permission letter here of the kind Király gave for PC-TALKER.

**How the code is executed has no bearing on this.** Emulating it "abstractly"
is not a shield — distributing MacinTalk's 21 KB would be infringement whether
Musashi runs it or a real 68000 does. What is freely shippable is *our*
emulator, host and driver; the engine comes from the user's own copy. That is
precisely how EchoTalk handles the Textalker images, and it is the only part of
the arrangement doing real legal work.

Local investigation is fine. Publication needs the question answered first.

---

## Where the audio stands — 2026-08-15

**PCM comes out of the 1984 engine.** 3.93 s of speech-shaped envelope for a
sentence that should take about that long. It is not intelligible yet; the
listener's description was "a bird flapping its wings real fast".

### Verified correct

* `bufferCmd` targets alternate **A, B, A, B** without a single repeat or skip,
  every one declaring length 3870. The double-buffer handshake is sound.
* Both `SoundHeader`s carry length 3870, rate `$56EE8BA3`, baseFrequency 60.
* `CALLBACK` is installed at `chan+8` and fires.
* The waveform table pointers at `$A4(a5)`..`$C0(a5)` are populated with real
  addresses inside the driver image.
* The frame buffer at `$42(a5)` contains non-zero frame data.

### The actual defect

**The oscillator phase increments at `(a5)` and `$2(a5)` are both zero.**

The inner loop accumulates `add.w (a5), d1` and `add.l $2(a5), d2`, masks them
to `$3FF03FF`, and uses the result to index the waveform tables. With zero
increments the phase never advances, so the table lookup returns the same value
forever — which is exactly what the captured audio shows: runs of ~190 identical
bytes at a constant offset from silence, separated by near-silence. That
alternation at roughly the pitch period is the "flapping".

The frame buffer is also **sparse** — the first three 8-byte frames are nearly
all zeros while later ones carry plausible values — so the suspicion is that a
stage of the phoneme-to-frame pipeline (`+$07DC`, `+$086A`, `+$182C`, `+$09E2`,
`+$0AA4`, `+$0D1E`, `+$1944`, `+$044A`, `+$1B70`, `+$2028`, `+$2284`, `+$2716`)
is not completing, rather than the oscillator being wrong.

### Two synthesis loops, not one

They disagree about stride, and both consult the channel pointer per sample:

| loop | with a channel | without |
|---|---|---|
| `+$2836` (interpolating) | `addq.l #2, a3` — writes the sample **and** a mean-interpolated midpoint | `#4` |
| `+$298E` | `addq.l #1, a3` — one byte per sample | `#2` |

So the engine's native rate is 11127.27 Hz in the first loop and 22254.5455 Hz
in the second. Do not assume one rate for the whole utterance.

### Also still open

`Prime` never returns — after the utterance it emits silent buffers forever.
`$9A(a5)` (characters consumed) reads 50 against a 49-character input with
`$D0(a5)` still 49, so the outer loop at `+$0316` looks like it *should*
terminate on the next pass and does not reach it.

### Reference: how Berkeley set the channel up

From `Ocod` segment 4, `+$5690`:

```
NewPtr($424)                  ; 1060 bytes -- a full SndChannel
move.w #$80, $1E(a0)          ; qLength = 128
SndNewChannel(&chan, 5, 0, NIL)   ; synth 5 = sampledSynth, no init flags
```

Our probe allocates 64 bytes and zeroes them, which is enough for `chan+8` and
`chan+$20` but is not what the engine was built against.

---

## The defect, located precisely — 2026-08-15

Three oscillator phase increments live at `a5+0`, `a5+2`, `a5+4` (word each).
A write watchpoint on those six bytes across a whole utterance shows **exactly
who writes them**:

```
driver+0x027E4   a5+1  x1     value 0     <- first frame only
driver+0x027EC   a5+3  x1     value 0     <- first frame only
driver+0x027F4   a5+5  x1     value 0     <- first frame only
driver+0x028E0   a5+4  x341   values 0,2,4,6,14,18   <- every frame
```

So the **pitch** increment is refreshed per frame, but the two **formant**
increments are loaded once, from frame 0, and never again. The frame loader at
+$27E4 is straight-line code fallen into from the per-utterance init at +$27B0;
nothing branches back to it.

**Frame 0 is all zeros.** Hence both formant oscillators sit at a fixed phase for
the entire utterance, the waveform lookup returns a constant, and the output
holds DC across each pitch period — which is what the capture shows and what the
listener heard as flapping.

### The frame buffer is not empty, which is the puzzle

Eight bytes per frame, `$42(a5)`, for "DHIHS KAA1PIY IHZ":

```
  #   osc1 osc2 pitch          #   osc1 osc2 pitch
  0     0    0    0            6     5   36   30
  1     0    6    5            7     6   42   36
  2     0    0   10            8     0    0   41
  3     2   18   15            9     7   54   46
  4     3   24   20           10     8   61   52
  5     0    0   25           11     0    0   57
```

Real, smoothly ramping data — but **every third frame has osc1 = osc2 = 0**, and
frame 0 is one of them. So either

* the generator is writing a three-frame cycle in which one frame is a
  deliberate transition and my 8-byte framing is misreading it, or
* the loaders disagree: +$27E4 consumes **8** bytes per frame while the
  steady-state loader at +$28DA consumes **6** (one, skip three, then two, with
  bytes 1-3 re-read backwards at `-$5(a6)`/`-$4(a6)`/`-$3(a6)` as formant table
  selectors). Those cannot both be right about the same buffer.

The 6-vs-8 disagreement is the most likely thread to pull next.

### Tooling note — a decoder bug that manufactured a red herring

`tools/disasm.py` clipped its byte slice at the requested end address, so the
last instruction in any range decoded from too few bytes and invented an
operand. A plain `move.b (a6)+, $5(a5)` read as `move.b (a6)+, -$5556(a5)`,
which sent me hunting a self-modifying relocation that does not exist. Fixed:
`end` now bounds the loop, never the decoder's input. **Check the byte count
before believing a strange displacement.**

---

## Measured, not inferred: playback drifts off the frame grid and then stalls

A read watchpoint on the frame buffer settles the 6-vs-8 contradiction by
watching the machine instead of arguing with the disassembly.

**Everything that WRITES the buffer uses an 8-byte stride**, confirmed live:

```
driver+0x01B92  frame+  0
driver+0x01B92  frame+  8   (+8)
driver+0x01B92  frame+ 16   (+8)   ... and so on, uniformly
driver+0x01E5A  frame+152   \  the transition smoother, reading
driver+0x01E5E  frame+160   /  pairs exactly 8 apart
```

**Playback does not.** Late in the run the only frame-buffer reads left are:

```
driver+0x0295A  frame+1534
driver+0x0294E  frame+1533
driver+0x02954  frame+1535
   ...the same three bytes, forever
```

Two things fall out of that:

1. **`a6` is at 1538, which is not a multiple of 8.** It has drifted off the
   frame grid, exactly as the 6-byte consumer stride predicts. So from some
   point onward the "formant selectors" are being read from the middle of the
   wrong frame.
2. **`driver+0x28DC` never fires again.** That is the frame-advance read, and
   without it `a6` stops moving entirely. The engine is stuck re-reading three
   bytes and emitting samples from them — which is both the constant output and
   the reason `Prime` never returns.

So the garbled audio and the non-termination are **one defect, not two**.

### Why it stalls

The frame advance at +$28DA is guarded by two counters packed into the halves
of `d0`, juggled by `swap`:

```
+028CE  subq.w #1, d0 ; bpl.b $2930     <- never falls through any more
+028D2  jsr $0.l                        <- stop-speech hook
+028DA  move.b (a6)+, d7                <- the frame advance
...
+02930  swap d0 ; subq.w #1, d0 ; bpl.b $2960
+02936  moveq #0, d1 ; moveq #0, d2     <- phases reset every sub-frame
+02948  clr.w d0 ; move.b $10(a5), d0   <- reload from the frame
```

`$38(a5)` is `$35B6 / rate` = 13750/150 = **91**, which at the engine's 11127 Hz
native rate is 8.2 ms — a correct frame period. The other counter comes from
`$10(a5)`, loaded from the frame's last byte. Once `a6` is off-grid that byte is
garbage, the counter never expires, and +$28DA is never reached again.

Note also +$2936: **`d1` and `d2` — the two formant phases — are zeroed on every
sub-frame.** With the increments also zero (loaded once, from an empty frame 0)
the phase can never accumulate at all.

### Next

Find the missing `a6 += 2`. The candidates, in order:
* an advance inside the `$14(a5) != 0` loop at +$298E that only runs when that
  flag is set — ours is always 0, which would mean our `$14` is wrong, not the
  stride;
* the stop-speech hook at +$28D2 being expected to adjust something (our stub
  only returns 0);
* `$10(a5)`/`$14(a5)` being read from the wrong byte offsets, which would make
  the "6-byte" reading itself an artefact of already-drifted alignment.

The third is the most likely: the drift may be a *consequence* of a wrong first
frame rather than its cause.

---

## The front end works. The engine overruns its own frame buffer.

Verified the three generator input lists for the first time (they are
`$100(a5)` phoneme indices, `$500(a5)` durations, `$300(a5)` flags). For the
input `DHIHS KAA1PIY IHZ` -- "this copy is" -- the staged phoneme list is:

```
 0  QX  dur 20     <- leading glottal
 1  DH  dur  5
 2  IH  dur 13
 3  S   dur 11
 4  KX  dur  8     <- K expanded to its aspirated allophone
 5  ..  dur  1        plus two burst components
 6  ..  dur  5
 7  AA  dur 19
 8  P   dur 10     <- P likewise
 9  ..  dur  1
10  ..  dur  2
11  IY  dur 13
12  IH  dur 14
13  Z   dur 10
14  -   dur 24     <- trailing pause
15  FF  END
```

That is a correct phonetic rendering with correct allophone expansion and
plausible durations. **Parsing, allophone selection and duration assignment all
work.** The hard half of the engine is fine; the defect is purely in playback.

### The overrun, provable from those numbers

Durations sum to **156 frames**, so `_NewPtr` allocates `156 * 8 + 8` = **1256
bytes**, and the frame list ends with the eight `$FF` bytes written at +$1CFC.

The read watchpoint caught playback still reading at **frame+1538**.

**1538 is 282 bytes past the end of a 1256-byte buffer.** The engine ran off the
end of its own data into unallocated heap, which is why it then read constants
forever and never returned. Not a subtle numerical problem -- a straightforward
overrun.

### So the real question is: what should have stopped it?

The **first**-frame loader checks the terminator:

```
+027E4  move.b (a6)+, $1(a5)
+027E8  bmi.w  $290C            ; byte >= $80 -> end of speech
```

The **steady-state** loader does not. Its only `bmi` tests the return value of
the stop-speech hook, not the frame byte:

```
+028D2  jsr    $0.l             ; the stop-speech callback
+028D8  bmi.b  $290C            ; hook says stop
+028DA  moveq  #0, d7
+028DC  move.b (a6)+, d7        ; pitch -- NOT checked for $FF
```

Our hook is `moveq #0,d0; rts` -- permanently "keep going". So either

* the hook is genuinely how an utterance ends, and Berkeley's returned negative
  once the frame list was consumed (in which case our stub is the bug and the
  fix is to make it count frames), or
* the terminator is caught in the **other** synthesis loop at +$298E, which runs
  when `$14(a5)` is non-zero -- and `$FF` in that field would select exactly
  that path. Our `$14(a5)` is 0 for every frame, which would mean we never get
  there because the field is being read from the wrong offset.

The second is the more likely, and it would also explain the 6-vs-8 stride
discrepancy: both are symptoms of reading `$14`/`$10` from the wrong bytes.
+$298E is the last significant block of this engine still unread.

---

## Two synthesis loops — and only one of them makes sound

A write watchpoint on the sound buffer's sample area answers the question I
should have asked first: **who actually writes the PCM?**

```
driver+0x02864  x184   distinct values 1    (just $80)
driver+0x0286E  x184   distinct values 2    ($40, $80)
driver+0x029CA  x72    distinct values 12   62 67 6C 71 76 7B 80 85 8A ...
driver+0x029EC  x72    distinct values 11   67 6C 71 76 7B 80 85 8A ...
```

**Loop A (+$2812, reached when `$14(a5)` is zero) writes nothing but silence.**
Every sample it produces is exactly `$80`; the `$40`s are its interpolated
midpoints at a buffer start. **Loop B (+$298E) produces a real waveform** —
evenly spaced levels five apart, which is what a table-driven formant
oscillator should look like.

And loop A runs 184 times for every 72 of loop B. That ratio *is* the artefact:
mostly silence, punctuated by short bursts of genuine speech, repeating at the
buffer rate.

### Why loop A is silent

The prologue sets up the tables:

```
+02746  lea $459E(pc), a0     ; phase -> index table
+0274A  lea $2BF6(pc), a1     ; the waveform table
+0274E  lea $369E(pc), a4     ; 8 waveform pointers, stride $1E0,
+02754  lea $A4(a5), a4       ; copied into a5[$A4..$C0]
```

Loop A then computes `d6 = d3 | a0[phase]` and looks up `a1[d6]`, twice, and
finishes with `addi.b #$80, d7`. **`d3` is a frame selector byte shifted left
5.** When that byte is zero, `d3` is zero, both lookups land on the table's
silence row, and the result is exactly `$80`.

So loop A is silent precisely when its selector byte is zero — and two of every
three frames have zeros in the columns it reads.

### The off-by-two, and evidence for it

The generator writes each frame as **low halves at bytes 0,1,2 and high halves
at bytes 3,4,5** (`move.b d3/d4/d5`, `swap`, `move.b d3/d4/d5` again). Loop A
reads bytes **1,2,3** — straddling the boundary between the two groups. That
alone would produce a mixture of real and zero selectors.

Shifting the three displacements at +$2950/+$2956/+$295C and re-measuring:

| selector bytes read | peak | distinct levels |
|---|---|---|
| 1,3,2 (as shipped) | 94 | 92 |
| 3,5,4 (high group, +2) | 111 | 151 |
| 0,2,1 (low group, -2) | **127** | **184** |

Reading either coherent group gives markedly more dynamic range and far more
distinct levels than the straddling read. That is real evidence the frame
alignment is off — though neither shift alone makes it intelligible, so the
offset is a symptom of something upstream rather than the whole fix.

### Ruled out, with measurements

* **Not the oscillator increments.** Forcing `a5+1`/`a5+3` to any value changes
  the output by exactly nothing (patches verified to land in emulated memory).
* **Not the sub-frame counter.** Forcing `$10(a5)` to 4, 8, 16, 24 or 40 changes
  the buffer count but not one sample value.
* **Not the frame stride alone.** Patching `addq.l #3,a6` to #4 or #5 at +$28E4
  does not help.

All three of those were my leading theories. Recording them as *dead* is worth
as much as the live lead.
