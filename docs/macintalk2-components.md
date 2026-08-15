# MacinTalk 2 — the shape of the thing

`.sp` is the engine outSPOKEN falls back to. **MacinTalk 2 is the one people
actually heard**, because outSPOKEN preferred it whenever it was installed and
outSPOKEN 8 dropped `.sp` entirely. Ten voices nobody has ported: Ben, Boris,
Brenda, Mariel, Marvin, Mr. Hughes, Otis, RoboVox, Votron, Xero.

This records what it is, before any host code exists. Everything below came out
of `tools/rsrc.py` and `tools/disasm.py`; nothing has been executed.

## It is two Components, not a driver

`.sp` was a `DRVR` with five entry points. MacinTalk 2 is a pair of Component
Manager components inside one extension:

| resource | size | role |
|---|---|---|
| `Cecy 1 'BACKEND'` | 23,234 | the synthesiser |
| `Cecy 3 'FRONTEND'` | 21,770 | text to phonemes |
| `ttsr 'Pronunciation Rules'` | 19,701 | rules |
| `ttsd 'Dictionary'` | 25,972 + 1,302 | exception dictionary |
| `ttss 'Phoneme Symbols'` | 2,270 | the phoneme alphabet |
| `ttph 'Magic Char Map'` | 128 | |
| `ttop 'Magic Opcode Map'` | 64 | |

The `thng` resources register them:

```
thng 1  'MacInTalk2 Back-End Prototype'   type 't2be' sub 't2be' manuf 'mtk2' -> Cecy 1
thng 3  'MacInTalk2 Front-End'            type 'ttsc' sub 'mtk2' manuf 'mtk2' -> Cecy 3
```

`ttsc` is the Speech Manager's synthesiser component type, so the front end is
what the Speech Manager talks to; `t2be` is Apple's own private back-end
interface, documented nowhere.

A voice is three resources — `ttvi` (info), `ttvd` (description, holds the
name), `ttvw` (data) — about 29 KB. That triple is also how you tell a
MacinTalk 2 voice from a MacinTalk 3 one, which has no `ttvi` at all.

## Both use the standard component entry

```
+0000  link.w   a6, #$0
+0004  movem.l  d7/a3-a4, -(a7)
+0008  movea.l  $c(a6), a3       ; a3 = ComponentParameters *
+0010  move.w   $2(a3), d7       ; d7 = params->what, the selector
+0014  bge.b    <component calls>
```

`pascal ComponentResult main(ComponentParameters *params, Handle storage)`
pushes left to right, so **storage is at 8(a6) and params at 12(a6)** — which
is what the disassembly does.

The complete API surface, read off the two jump tables:

| | standard | component | handlers |
|---|---|---|---|
| back end | −1…−6 | **0–7** | +$320 +$388 +$388 +$3EA +$7A6 +$42C +$518 +$2D8 |
| front end | −1…−6 | **0–9** | +$5CE +$3F2 +$4B0 +$506 +$554 +$FAA +$11AE +$7C4 +$596 +$1482 |

Anything outside those ranges returns `$80008002` — `badComponentSelector`.

## The host has to supply the Component Manager glue

This is the part that differs most from `.sp`, and it is not optional. A
component does **not** call its own handler. It hands the handler back to the
Component Manager:

```
+00F8  subq.l  #$4, a7          ; room for the result
+00FA  move.l  $8(a6), -(a7)    ; storage
+00FE  move.l  a3, -(a7)        ; params
+0100  move.l  a4, -(a7)        ; the selected handler
+0102  moveq   #$FF, d0         ; D0 = -1
+0104  A82A                     ; _ComponentDispatch
+0106  move.l  (a7)+, $10(a6)   ; result
```

So `$A82A` with `D0 = -1` is `CallComponentFunctionWithStorage`, and **we**
must unpack `ComponentParameters` into real arguments and call the handler.
`ComponentParameters` is `UInt8 flags; UInt8 paramSize; SInt16 what; long
params[]`, so `paramSize` says how many bytes of arguments to push.

The front end also calls `$A82A` with `D0 = $10` and `D0 = $0E` — the storage
accessors. The exact numbers are worth confirming empirically rather than from
memory: the host should log every `$A82A` selector it sees on the first run,
exactly as the `.sp` trap log settled that engine's surface in one pass.

## Two roles for the host

* **Speech Manager** — the caller. It opens the `ttsc` front end and issues
  speak calls, the same way the host played Device Manager for `.sp`.
* **Component Manager** — the switchboard. It answers `$A82A`, and when the
  front end opens `t2be` it instantiates the back end from `Cecy 1`.

The private `t2be` interface never has to be understood. The front end's own
`$A82A` call sites are its author using it correctly; we route them.

## Toolbox surface

A whole-file scan finds 58 distinct A-traps in the back end and 18 in the front
end. Treat that as a **ceiling, not a spec** — the scan walks data as code, and
`$AAAA` appearing three times is almost certainly a data pattern. The real
surface is whatever the first run's trap log reports.

Two that matter now:

* `$A804` `_SndDoImmediate` and `$A800` `_SoundDispatch` — **audio leaves the
  same way `.sp`'s does**, so the existing sound model carries over.
* `$A89F` is `_Unimplemented`, and it appears beside `_GetTrapAddress`. That is
  the classic `TrapAvailable()` pattern: compare `GetTrapAddress(trap)` with
  `GetTrapAddress(_Unimplemented)`. The host must return a **unique** address
  for `_Unimplemented` and **distinct** addresses for traps it wants reported
  present, or feature detection silently takes the wrong branch.

## Scope

* **MacinTalk 3 is excluded.** WinTalker builds that engine natively from
  Apple's source; emulating it would be strictly worse.
* **MacinTalk Pro is deferred.** Different engine, 800–935 KB concatenative
  voices.
* **Distribution is unchanged.** MacinTalk 2 and its voices are Apple's, they
  are user-supplied through `rom/`, and `tools/extract_rom.py` pulls them from
  the user's own disk image. Nothing here or in any release contains them.
* This lands in the **same host and repo** as `.sp`. The component registry is
  an addition to `osp_host.c`, not a fork.

## Milestones

1. Host calls the front-end entry with selector −1 (open), then −4 (version).
2. Watch it `OpenComponent('t2be')` through our `$A82A` handler; instantiate
   the back end from `Cecy 1`.
3. Register `ttsr`/`ttsd`/`ttss`/`ttph`/`ttop` and one voice's
   `ttvi`/`ttvd`/`ttvw` in the existing resource registry; issue a speak call.
4. Audio exits via `$A804`/`$A800`, which the host already models. Probe → WAV
   → Tomi's ear.

Same shape as `.sp`, and the snapshot breakpoint and watchpoints transfer
unchanged.
