# How `.sp` makes sound — settled

**The gate question is answered.** MacinTalk does not drive the DAC and does not
open the Sound Driver. It fills two buffers with 8-bit PCM and hands them to the
**Sound Manager** with `bufferCmd`, using a `callBackCmd` to learn when a buffer
has drained.

This is the best of the possible answers. It means the host models **no timing at
all** — no VBL, no 22 kHz interrupt, no DAC pacing. We answer a trap, take 3870
bytes, and say "done". The engine runs as fast as the emulator will carry it.

Everything below was read out of `DRVR 1030` with `tools/symbol_map.py` and
`tools/disasm.py`. No emulator was involved.

---

## The numbers

| thing | value | where |
|---|---|---|
| sample rate | `$56EE8BA3` Fixed = **22254.5455 Hz** | `MACSTARTSOUND` +$5146 |
| buffer length | `$0F1E` = **3870 samples** — but see below, the driver rewrites it | `MACSTARTSOUND` +$513A |
| fill limit | `sampleArea + $E73` = 3699 (= 10 × 370, ten classic sound frames) | `StuffA3` +$52EA |
| buffer duration | **173.9 ms** | derived |
| sample format | **8-bit unsigned**, silence = `$80` | `ClearBuffers` fill value |
| baseFrequency | `$3C` = 60 (kMiddleC) | `MACSTARTSOUND` +$5152 |
| encode | 0 (`stdSH`) — never written, block arrives cleared | — |
| samples live at | header **+$16 (22)**, i.e. `samplePtr` is NIL | `ClearBuffers`, `+$52BC` |
| buffers | **two**, at globals `+$14` and `+$18` | throughout |

`22254.5455 Hz` is `rate22khz`, the classic Mac rate. It is not a coincidence and
not a guess: `$56EE8BA3 / 65536` is exactly that number.

## The play loop

`SetupA3` and `EndOfSpeech` both issue the same pair, `noWait = 1`:

```
move.w  #$51, -$8(a6)        ; bufferCmd  (81)
move.l  $14(a4), -$4(a6)     ; param2 = &SoundHeader
move.l  $10(a4), -(a7)       ; the channel
pea     -$8(a6)              ; &SndCommand
move.b  #1, -(a7)            ; noWait
A803                         ; _SndDoCommand

move.w  #$0D, -$8(a6)        ; callBackCmd (13)
move.w  #$04, -$6(a6)        ; param1 -- identifies which buffer
A803
```

`CALLBACK` (+$50C4) is installed into the channel's `callBack` field at `+8` by
`MACSTARTSOUND`. All it does is test `param1 & 4` and set bit 4 or bit 8 of the
flag word at globals `+$2`. The synthesiser then spins on that word at `+$52BC`,
clears it, and repoints its output cursor `$48(a5)` at the other buffer's sample
area `+ $E73`.

So the whole handshake is one word of shared state. **We can satisfy it by
calling `CALLBACK` the moment we have copied a buffer out** — or, if that ever
proves awkward, by setting the flag word directly. Calling the real routine is
preferable and no harder.

## The rest of the Sound Manager surface

Small, and all of it selector-clean:

| routine | call | meaning |
|---|---|---|
| `ChannelBusy` | `_SoundDispatch` `$A800`, D0 = `$00100008` | `SndChannelStatus`; reads `scChannelBusy` at status`+12` |
| `SilenceChannel` | `_SndDoImmediate` `$A804`, cmd 4 then cmd 3 | `flushCmd`, then `quietCmd` |
| `WaitSoundDone` | `_TickCount` `$A975` + `ChannelBusy` | spins, **one-second cap** (`+$3C` ticks) |
| `MACSTOPSOUND` | same two | also one-second capped |
| `OpenSound` / `CloseSound` | `link a6,#0 / unlk / rts` | **empty stubs, 8 bytes each** |

Every wait in this driver is bounded by `TickCount + 60`. Nothing here can hang
the host waiting for audio that never drains — which also means a host that
answers instantly is not just allowed, it is the fast path the code was written
to tolerate.

## `$48(a5)` is a limit, not a cursor — and `A3` is the cursor

This is the one number that looked wrong. `$E73` = 3699 sits only 171 bytes from
the end of a 3870-byte buffer, which makes no sense for a write pointer. It is
not one. Searching the whole synthesiser for `$48(a5)` finds exactly two uses,
both at +$287C and +$2A02, and both are:

```
cmpa.l  $48(a5), a3
```

**`$48(a5)` is the end-of-fill limit. `A3` is the cursor** — which is precisely
what the routine name `StuffA3` has been saying all along. So:

```
a3        = sampleArea            ; cursor, fills forward
$48(a5)   = sampleArea + $E73     ; stop when a3 reaches here
```

The fill is **forward and in order**. The 171 bytes past the limit are slack, so
the final chunk can overrun without corrupting anything — `ClearBuffers`
pre-fills the whole area with `$80`, so any slack that goes unwritten is silence.

## Re-read `length` at every `bufferCmd` — do not hardcode 3870

`SetBufLength` at +$4C36 rewrites the header:

```
d7 = globals[$0C]        ; where the synthesiser actually stopped
globals[$0C] = 0
a3 = (index == 1) ? bufferA : bufferB
a0 = a3 + $16            ; sampleArea
d7 = d7 - a0             ; bytes actually written
if d7 > $F1E:            ; clamp, and flip a byte through ScrnBase ($0824)
    d7 = $F1E
header[$4] = d7          ; SoundHeader.length := the real count
```

`ResetBufLength` (+$4CA0) puts `$F1E` back for the next full buffer. Both are
called with the same 1/2 buffer index, from `SetupA3` and `EndOfSpeech`.

So the **last buffer of every utterance is short**, and its true length lives in
the header. A host that always takes 3870 bytes appends up to 170 bytes of
whatever the buffer held to the end of each utterance.

**Diagnostic, so nobody loses an evening to it:** hardcoding the length gives a
regular low-frequency chop or buzz under the voice — the buffer period is
173.9 ms, so the artefact lands near 6 Hz.

The clamp branch is worth knowing too: on overrun the original **XORs a byte at
`ScrnBase`**, flashing eight pixels in the corner of a 1988 Macintosh screen.
That is a debugging aid left in the shipped binary. If our host ever takes that
path, something has gone wrong upstream — log it rather than emulating the
pixels.

The same shape appears in the fallback path with the same one-chunk margin:

```
tst.l   $10(a0)            ; a channel?
bne     .modern
movea.l $0266.w, a3        ; no -- the raw sound buffer
addi.l  #$171, ...         ; limit = base + 369, one short of 370 words
```

That is the original 1984 direct-DAC path, preserved intact. **If our host ever
produces silence, check first that the channel pointer at globals `+$10` is
non-zero** — a zero there sends the synthesiser down this path, writing into low
memory, and the symptom is silence indistinguishable from a broken program.

## The host allocates the buffers — the driver never does

`MACSTARTSOUND` does not create anything. It reads them out of a record it is
handed:

```
movea.l $8(a6), a3          ; the caller's record
move.l  (a3),   $10(a4)     ; the sound channel
move.l  $4(a3), $14(a4)     ; buffer A
move.l  $8(a3), $18(a4)     ; buffer B
```

The only allocations in the entire 21 KB image are `_NewHandle($B00)` in
`DriverOpen` (that is `dCtlStorage`, the settings block) and one `_NewPtr` at
+$1CCE sized `8n+8` from a phoneme string, stored at `$42(a5)`. **Neither is a
sound buffer.** Berkeley's own code allocated the channel and both buffers and
passed them in.

So our host must supply: a sound channel, and two blocks of at least
`22 + 3870 = 3892` bytes. Nothing in the driver will ask for them.

## What the host must implement

Small enough to list completely:

* `_SndDoCommand` `$A803` — on `bufferCmd`, read the `SoundHeader` at param2 and
  copy **the `length` found in that header** (not 3870) from header`+$16`; on
  `callBackCmd`, invoke `CALLBACK`.
* `_SndDoImmediate` `$A804` — `flushCmd` / `quietCmd`: drop pending audio.
* `_SoundDispatch` `$A800` selector 8 — fill `scChannelBusy` at `+12`.
* `_TickCount` `$A975` — a monotonic 60 Hz counter. It only ever feeds timeouts.
* `_NewHandle` / `_NewPtr` / `_HLock` — `DriverOpen` wants **`$B00` = 2816 bytes**
  for `dCtlStorage` at DCE`+$14`.
* `_GetResource` / `_ReleaseResource` / `_SetResLoad` / `_OpenResFile`, plus
  `ResErr` at low memory `$0A60`.

Note the resource IDs. `DriverOpen` asks for **`RULZ` 129, `RULZ` 130 and
`TALK` 1**; what outSPOKEN actually carries is **`RULZ` 1129** and **`TALK`
1001** — offset by exactly 1000. The host will need to map them, and we are one
`RULZ` short of what the probe wants. That probe block is not reached from
`DriverOpen`'s fall-through path, so it is likely a `Control` csCode; worth
confirming before treating the missing resource as a problem.

## What the host does **not** need

No VBL task. No `_VInstall`. No interrupt model. No DAC. No 22254 Hz pacing. No
Sound Driver. The `WAVE` resources in outSPOKEN (`Bonk`, `Humming`, `At Work`)
are the control panel's own UI sounds and have nothing to do with `.sp`.

---

## Provenance note

This driver is a **1984 assembly core wearing a 1988 Pascal jacket**. The two are
visibly different: the core is hand-written 68000 with `A5`/`A3` globals and
writes its MacsBug names as `$80|len, len, name`; the glue is compiled Pascal with
`link a6` frames, Toolbox calls pushed Pascal-style, and names written as
`$80|len, name`. `tools/symbol_map.py` accepts both, which is how it finds all 18.

The practical consequence: **the Sound Manager layer is Berkeley's 1988 work, not
Katz and Barton's.** The synthesiser underneath it is the 1984 engine, and it
still speaks to its output through a cursor in `$48(a5)` exactly as it would have
on a 128K Mac. We are emulating a 1984 synthesiser through a 1988 porch, and only
the porch touches the Sound Manager.
