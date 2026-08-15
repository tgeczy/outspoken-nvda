# Unicorn cannot run this. The core has to be Musashi.

## What happened

MacinTalk's `Open` routine, at `DRVR +0x005A`:

```
+005A  61 00 4E 28    bsr.w   OpenSound
+005E  48 E7 00 E0    movem.l a0-a2,-(a7)      <-- exception 4, illegal
+0062  42 68 00 32    clr.w   $32(a0)
```

The second instruction faults. Not an exotic one — `movem.l regs,-(a7)` is the
standard 68000 routine prologue. Nearly every routine in every 68k program
starts with it.

## Why

**Unicorn's m68k target comes from QEMU, which grew up around ColdFire, and
ColdFire removed exactly these instructions.** Measured, not assumed:

| instruction | result |
|---|---|
| `movem.l a0-a2,-(a7)` | **exception 4** |
| `movem.l (a7)+,a0-a2` | **exception 4** |
| `movem.l d0-d7,-(a7)` | **exception 4** |
| `movem.l (a0),d0-d1` | ok — the form ColdFire kept |
| `dbra d0,-4` | **exception 4** |
| `link` / `unlk` / `bsr.w` / `rts` | ok |
| `lea (pc)`, `swap`, `ext.l`, `clr.w (d16,An)` | ok |

`rts` looked broken in the first pass and was not — that was the test harness
returning into unmapped zeros. Worth re-checking before drawing a conclusion;
the real list is `movem` predecrement/postincrement and `dbcc`.

Losing those two is not a corner case. It is most of the code.

## What Unicorn IS good for here

The gate questions it answered still stand and still save work:

* **A-line traps dispatch cleanly.** A Mac Toolbox call surfaces as
  `UC_HOOK_INTR` with `intno=10`, PC parked on the `$Axxx` word. Read it,
  service it, advance PC by 2. That mechanism carries over to any core.
* **`illegal` loops** — PC does not advance, so it fires forever. Terminate on
  a sentinel address instead, the way `pctalker_speaker.py` does.

## The core to use

**Musashi** — Karl Stenerud's 68000 interpreter, the one MAME ships. Mature,
accurate for the 68000 specifically, plain C, MIT licensed, and designed to be
embedded behind callbacks for memory access.

This is the same shape as EchoTalk: Jayson vendored `fake6502`, a public-domain
6502 core in C, wrapped it, and built a DLL the NVDA driver loads with `ctypes`.
Ours would vendor Musashi instead, for a different CPU, and expose the same kind
of small C API. `the WinTalker project` already proves the toolchain on this machine —
CMake plus MSVC, building `WinTalker.dll` for both x86 and x64.

Nothing about the plan changes except which core sits underneath. The A-trap
dispatch, the Sound Driver model, the resource server and the NVDA driver are
all unaffected.

## What does not change

The rights position: Musashi is MIT and ships freely, our code ships freely,
and **`outspoken.bin` is supplied by the user**. That is exactly how EchoTalk
handles the Textalker images, and it is the only part of the arrangement that
actually does legal work — how the code is executed is irrelevant.
