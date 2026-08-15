# The frame stream — settled

MacinTalk's synthesiser is driven by a stream of **8-byte frames** in the block
at `$42(a5)`, allocated by the single `_NewPtr` at +$1CCE with size `8n+8`.

Everything below was measured with `tools/probe_frames.py` and the snapshot
breakpoint, not deduced from a listing. That distinction earned its place: the
frame stride was read wrongly off the disassembly three separate times, and
each wrong answer was plausible enough to spend hours on.

## The frame

| byte | field | consumer |
|---|---|---|
| `f[0]` | formant 1 increment, low half → `$1(a5)` | **the callback** |
| `f[1]` | formant 2 increment, low half → `$3(a5)` | **the callback** |
| `f[2]` | formant 3 increment → `$4(a5) = f[2] * 2` | loader |
| `f[3]` | formant 1 amplitude → `d3 = f[3] << 5` | +$294E |
| `f[4]`, `f[5]` | formant 2 and 3 amplitudes → `d4 = (f[5]<<16 \| f[4]) << 5` | +$294E |
| `f[6]` | voicing selector → `$14(a5)`; zero picks loop A | loader |
| `f[7]` | overall amplitude → `$10(a5)` | loader |

`$0(a5)` and `$2(a5)` hold the high halves of the increments and are *not*
rewritten per frame — the loaders only ever touch the odd bytes `$1`, `$3` and
the word at `$4`. `d1` is formant 1's phase; `d2` is a long holding formants 2
and 3 packed into its halves, which is why loop A `swap`s it.

**Bit 7 of `f[0]` terminates the utterance.** The first-frame loader shows the
contract plainly at +$27E4: `move.b (a6)+, $1(a5)` followed by `bmi.w $290c`,
straight to the end-of-speech `rts`.

## The callback is part of the loader

This is the part that is easy to get wrong, and it is worth stating flatly:

> **The routine installed by `SetStopSpeechCallback` (driver+$0034) is not a
> notification. It reads the first two bytes of every frame, and the
> synthesiser cannot advance without it.**

The steady loader at +$28DA consumes **6** bytes. Frames are **8**. The
callback, invoked through the self-modifying `jsr $0.l` at +$28D2, runs once
per frame immediately before it and consumes the other two.

The proof is at +$294E, which reads the formant amplitudes as `-$5(a6)`,
`-$4(a6)` and `-$3(a6)`. Those are `f[3]`, `f[4]`, `f[5]` **only if `a6` has
reached `f+8`**. Six plus two is eight. With that, every remaining loader read
lands on the same field the first-frame loader extracts by hand — `f[2]` to
`$4(a5)`, `f[6]` to `$14(a5)`, `f[7]` to `$10(a5)`. The two paths agree
completely, which they did not under any other reading.

The minimum viable callback, and what `tools/probe_speak.py` installs:

```
move.b  (a6)+, $1(a5)     ; f[0]; bit 7 set = end of speech
move.b  (a6)+, $3(a5)     ; f[1]
tst.b   $1(a5)            ; the second move clobbered N -- put it back
rts
```

The `tst.b` matters. The caller does `jsr` then `bmi`, so **N on return is the
stop flag**, and it must reflect `f[0]`, not whatever the last instruction set.

## What a wrong callback looks like

Worth recording, because the symptom does not point at the callback at all. A
stub that always reports "keep going" (`moveq #0,d0; rts`) leaves the stream
two bytes short per frame. The cursor drifts off the 8-byte grid, never lands
on a terminator, and walks straight off the end of the block into unallocated
heap, synthesising whatever it finds there.

Measured, same phonemes, wrong callback then right one:

| | wrong | right |
|---|---|---|
| buffers taken | 2167 | 7 |
| duration | 376.94 s | 1.22 s |
| non-silent samples | 0.3% | 56.7% |
| `a6` stride | 6 | 8 |
| how it ended | instruction budget | `_DisposePtr` at +$392 |

None of those numbers say "callback". They say "the audio is garbage and it
never stops", which is why this cost what it did.

## Dead ends, so nobody re-walks them

Three readings of this stream were wrong, and all three were arrived at by
staring at the disassembly:

* **6-byte frames with a 2-byte preamble.** Reconciles the two loaders' field
  offsets perfectly on paper. The pristine dump kills it — the parameter
  tracks only cohere at period 8.
* **The generator straddles two byte-groups**, so loop A reads bytes 1,2,3
  across a low-half/high-half boundary. The byte *grouping* was right; the
  interpretation was not. `f[0..2]` are frequencies and `f[3..5]` amplitudes,
  and nothing straddles anything.
* **`a6` lands at the wrong offset.** It does not. `a6` is seeded from
  `$42(a5)` at thirteen sites and is correct at every one.

The tell that beat all three: dump the block *before* playback touches it (the
player clears three bytes of every frame it consumes at +$28EC, so a post-run
dump describes playback, not its input) and lay the bytes out in 8-byte rows.
The three interpolating tracks are obvious — `(f[0]<<8)|f[1]` rises by exactly
262 per frame — and invisible at any other period.
