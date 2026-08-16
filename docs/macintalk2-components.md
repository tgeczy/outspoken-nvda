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

### Storage is the handler's *first* argument — measured, not recalled

The one number the glue cannot guess is where storage sits in the handler's
frame. Four handlers, read across both components, all agree: **the storage
handle is at the highest argument offset**, so it is pushed first and is the
first declared Pascal argument. With `N` unpacked arguments the frame is

```
$8(a6)          last-declared argument
  ...
$8+4(N-1)(a6)   first-declared argument
$8+4N(a6)       the storage Handle          <-- always the deepest
$8+4(N+1)(a6)   the result slot
```

| handler | args | storage at | result at |
|---|---|---|---|
| front end `+$596` | 1 | `$c(a6)` | `$10(a6)` |
| front end `+$5CE` | 1 | `$c(a6)` | `$10(a6)` |
| front end `+$3F2` | 3 | `$14(a6)` | `$18(a6)` |
| back end `+$320` | 1 | `$c(a6)` | `$10(a6)` |

Each one proves it the same way — `movea.l $X(a6),a0` immediately followed by
`movea.l (a0),a3`, dereferencing a handle, and then long offsets into the block
(`$4`, `$30`, `$34`, `$1db` in the front end; `$84`, `$1bc`, `$ac4` in the back
end). Nothing else in the frame is dereferenced twice.

So `CallComponentFunctionWithStorage(storage, params, handler)` must push
`storage` first, then `params->params[]` **verbatim** — copying the bytes in the
order they already sit in avoids having to decide anything about argument order.

### `$A82A` with `D0 = 0` is the call-another-component path

`D0 = -1` is the component calling its own handler. `D0 = 0` is the front end
calling the back end, and it builds `ComponentParameters` as an immediate
longword directly on the stack rather than passing a pointer:

```
+005A6  subq.l  #$4, a7              ; result slot
+005A8  move.l  $4(a4), -(a7)        ; the target ComponentInstance
+005AC  move.l  $8(a6), -(a7)        ; params[0]
+005B0  move.l  #$00040004, -(a7)    ; flags 0, paramSize 4, what 4
+005B6  moveq   #$0, d0
+005B8  A82A
```

giving the host this stack at the trap:

```
SP+0                 flags, paramSize, what   (the ComponentParameters header)
SP+4                 params[], paramSize bytes
SP+4+paramSize       the target ComponentInstance
SP+8+paramSize       the result slot
```

**`$4(<storage>)` is where the front end keeps the back-end instance it
opened** — so once our `OpenComponent` hands back a token and the front end
stores it there, every later back-end call names it from that slot.

One of these is already legible without touching the private `t2be` interface:
`+$5CE` forwards `'stat'` — `soStatus` — to back-end selector 5, and fills a
caller-supplied struct at `+0` byte, `+1` byte, `+2` long, `+6` word. That is
`SpeechStatusInfo` field for field, so `+$5CE` is the front end's
`GetSpeechInfo`. Apple's Speech Manager selectors are the vocabulary here, and
the `so····` four-character codes are the authority for them.

**`moveq #$FF,d0` sign-extends**, so the host must compare `D0` against
`0xFFFFFFFF`, not `0xFF`. And any `$A82A` selector the host does not recognise
must halt loudly with the stack dumped, never be stubbed — an unbalanced stack
here returns the caller into rubbish, which is exactly how the `.sp` port's
first `_GetResource` stub showed up.

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

* **MacinTalk 3 is excluded.** An existing NVDA add-on builds that engine
  natively from Apple's own source; emulating it would be strictly worse. This
  is a technical judgement, not a gap: native beats emulated, so those
  seventeen voices are better served where they already are.
* **MacinTalk Pro is the follow-on, and it is the same shape as this.**
  Measured 2026-08-15, not recalled: `thng 128 'Gala Tea'` declares type
  `ttsc`, manufacturer `gala`, code in `gtse 1` — 35,202 bytes of 68000 that
  opens with the *identical* standard component entry, `movea.l $c(a6),a3`
  then `tst.w $2(a3)` on the selector. So Pro is one `ttsc` component rather
  than a pair, its tables are the other `gtse`/`gtst` resources (~190 KB) and
  its lexicon is the 572,928-byte data fork. **Every line of Component Manager
  glue written for MacinTalk 2 serves Pro unchanged**, which is the reason to
  build that glue properly rather than narrowly.
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
