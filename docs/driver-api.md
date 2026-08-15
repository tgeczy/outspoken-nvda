# The `.sp` driver API — how to make it talk

Read out of `DRVR 1030` with `tools/disasm.py`. Companion to
`docs/sound-model.md`, which covers how the audio gets out.

## Speech comes through `_Write`, not `_Control`

The header settles it before any disassembly: `dWritEnable = 1`,
`dReadEnable = 0`, `dCtlEnable = 1`, `dStatEnable = 0`. And the code agrees —
`Control` at +$018E is a pure settings dispatcher with no path to
`MACSTARTSOUND`, while `Prime` (+$4BA4, the `_Write` entry) runs the synthesiser
and finishes with `jsr EndOfSpeech`.

**`PBWrite` the text. That is the whole speech call.**

An earlier note in this repo said Control was "the entry to drive for speech".
It is not; that was a guess and it was wrong.

## `Control` — the settings, with ranges and defaults

`csCode` comes from `$1A(a0)`, the value from `$1C(a0)` — the standard
`ioParam` offsets. `a0` is then the locked `dCtlStorage` from DCE `+$14`.

| csCode | range | live field | default | meaning |
|---|---|---|---|---|
| 0 | 0 or 1 | `$4C` | 0 | mode toggle |
| 1 | — | — | — | no-op, returns immediately |
| 2 | 0 … `$1000` (4096) | `$32` | **150** | **rate**, words per minute |
| 3 | 0 or 1 | `$3A` | 0 | **voice select** |
| 4 | `$41` … `$1F4` (65 … 500) | `$30` | 110 / 250 | **pitch**, Hz |

Out-of-range values are **silently ignored** — the code branches to the exit
without writing anything and still returns `noErr`. A host that sets a bad rate
gets no error and no change.

### Two voices, each with its own saved settings

`csCode 3` is not a voice *parameter*, it is a bank switch. `DriverOpen`
initialises both banks:

```
voice 0:  pitch $6E = 110   rate $96 = 150   toggle 0     ($C4/$C6/$C8)
voice 1:  pitch $FA = 250   rate $96 = 150   toggle 0     ($CA/$CC/$CE)
```

Selecting a voice copies that bank into the live fields; setting pitch or rate
writes both the live field *and* the current bank. Out of the box the two banks
differ only in pitch — **110 Hz and 250 Hz**, the male/female pair.

**The third field selects a formant table, and the second one does not work.**
`$3A(a5)` (`$C8`/`$CE` in the banks) has two consumers. The important one is in
the *generator*, at +$1D7A, where it chooses which table the frame builder
reads its three formant bytes from:

```
tst.w   $3a(a5)
beq.b   $1d84
lea     $4b9e(pc), a3     ; non-zero -> a different table
                          ; zero     -> $2ff6(pc)
```

It is read again at +$27BE, where it picks the sub-frame reload: 11 when zero,
8 otherwise.

**Setting it does not give a second voice.** Measured on the same sentence at
the same rate, `$3A = 0` renders 2.23 s and `$3A = 1` renders **0.18 s** -- the
utterance collapses almost immediately, which is what the terminator being hit
early looks like. Lowering the rate to compensate for the faster reload
(232 -> 169) only stretches it to 0.28 s. It is not a fast voice; it is a
broken render, and the "chipmunk" quality earlier attributed to it was that.

The alternate table is presumably indexed differently and needs something this
build does not configure. `DriverOpen` sets `$C8` and `$CE` to zero, so the
shipped product never selects it.

**So the voice list really is two: male at 110 Hz and female at 250 Hz.** The
Amiga narrator's four -- male/female against natural/robotic -- come from a
`mode` its own device supports; this Macintosh build either lacks it or never
shipped it working. Measured fundamentals for the two that do work, taken from
the rendered audio by autocorrelation rather than assumed:

| pitch field | measured f0 |
|---|---|
| 110 | 111.8 Hz |
| 250 | 247.3 Hz |

**Flat intonation has not been located.** The pitch path is `$30` (Hz) →
`d0 = $127690 / pitch`, clamped to `$4A38`, stored at `$34(a5)`; then per frame
`move.l $34(a5), d7 / divu.w d5, d7` writes a byte into the frame at +$21F2.
The per-frame variation in `d5` is the contour, so a monotone mode means
holding `d5` constant. Dropping stress digits from the phoneme string narrows
the spread but does not flatten it (sd 53.8 → 48.6 Hz), so the stress marks are
a contributor rather than the mechanism. Unresolved.

Note the storage is reached through a **handle**: `dCtlStorage` at DCE`+$14`
must be dereferenced before these offsets mean anything. Writing to the handle
itself changes nothing and produces four identical renders, which is a quiet
enough failure to cost an hour.

This maps onto NVDA cleanly: `voice` → csCode 3, `rate` → csCode 2,
`pitch` → csCode 4. Note the NVDA rate/pitch sliders are 0–100 and will need
scaling into 0–4096 and 65–500.

## `Prime` needs low memory `$012F` set to 0

The first thing `Prime` does after checking `dCtlStorage`:

```
move.b  $12F.w, d0        ; CPUFlag
cmpi.b  #2, d0
blt     $2EE              ; 68000/68010 -- straight to the synthesiser
...
bsr     $4BCC             ; 4E7A 0002 = movec CACR,d0   (68020+)
bsr     $2EE              ;   ...the synthesiser...
bsr     $4BE2             ; 4E7B 0002 = movec d0,CACR
jsr     EndOfSpeech
```

`$012F` is `CPUFlag` (0 = 68000, 1 = 68010, 2 = 68020, …). The cache save and
restore around the synthesiser only happens on `$012F >= 2`.

**Set `$012F` to 0 and no 68020 instruction is ever executed.** This is worth
more than it looks: it means a plain **68000** core is sufficient, and `movec` —
which Musashi implements but a minimal core might not — never comes up. It also
tells us the synthesiser is self-modifying, or at least was thought to be.

The real synthesiser entry is `$2EE`, immediately after `Status`.

`Prime` returns `$E4` (`-28`, `notOpenErr`) if `dCtlStorage` is nil.

## `Open` — what it wants before it will run

1. `bsr OpenSound` — an 8-byte empty stub, does nothing.
2. `_NewHandle($B00)` → stored at DCE `+$14` as `dCtlStorage`. 2816 bytes.
   Failure here aborts the open.
3. `_CmpString` (case- and diacritic-insensitive, `$A63C`) of a name against a
   constant, then possibly `_OpenResFile`, with `ResErr` at `$0A60` checked
   after.
4. `_GetResource('TALK', 1)`.
5. Initialise both voice banks to the defaults above.

### The `RULZ` probe is dead code — resolved, not a blocker

A block at +$00B6 probes `RULZ` 130 and `RULZ` 129 with `_SetResLoad(false)` /
`_GetResource` / `_SetResLoad(true)`, returning `$FF40` if either is missing.
Since `outspoken.bin` carries only one `RULZ`, this looked like it might stop the
engine dead.

**Nothing reaches it.** A sweep of every branch target in the image finds no
caller. The only apparent ones — at +$004C through +$0054 — are the
disassembler decoding the ASCII of `SetStopSpeechCallback` as instructions;
that string occupies +$0042 through +$0057. Once the string is excluded, the
block is unreachable from `Open`, from `Control`, and from the synthesiser.

Worth remembering as a method: **a branch-target sweep over a mixed code-and-data
image manufactures callers out of text.** Check whether a "caller" lands inside a
known string before believing it.

So the only resource the engine actually loads is `TALK 1`.

### The resource-ID gap

`Open` asks for `TALK 1`; what `outspoken.bin` carries is `TALK 1001` — offset by
exactly 1000, the same convention that puts the `DRVR` itself at id 1030. The
host should map ID → ID+1000 when serving `_GetResource`.

## The export table at +$0014

Berkeley's application code does not only go through the Device Manager. The
driver opens with a table of offsets and a bank of `jmp`s:

```
+$0014  dc.w  $001E, $0022, $0026, $002A, $000E
+$001E  jmp   MACSTARTSOUND
+$0022  jmp   MACSTOPSOUND
+$0026  jmp   STOPSPEECH
+$002A  get   stop-speech callback   (from the global at +$28D4)
+$0034  set   stop-speech callback   (into the global at +$28D4)
```

`MACSTARTSOUND` takes one pointer to a three-field record — `{channel, bufferA,
bufferB}` — which is how the caller-supplied buffers get in (see
`docs/sound-model.md`).

**This is the handle our host drives.** Call +$001E with the record to hand over
the channel and buffers, `PBWrite` to speak, `PBControl` to set voice, rate and
pitch, +$0026 to stop.

