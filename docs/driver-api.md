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
writes both the live field *and* the current bank. So the two voices differ, out
of the box, only in pitch — **110 Hz and 250 Hz**. That is the male/female pair,
and it is the entire voice list this engine has.

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

