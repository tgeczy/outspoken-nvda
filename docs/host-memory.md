# The host's Memory Manager — one heap, and what "dispose" has to mean

The host is a Macintosh just large enough to run five speech engines, and
its Memory Manager is correspondingly small: one zone, one flat heap, blocks
handed out from a moving top. That was enough for every engine to open, take
a voice and speak — and it hid a bug that shipped in every release up to
1.2.3 and that no test, no listener and no user ever reported.

## What the engines allocate

Measured on 2026-09-19 through the render oracle (`tools/render_oracle.py`),
speaking the same sentence repeatedly on a fresh engine and watching
`osp_heap_used()` after each utterance:

| engine | per utterance | how |
|---|---|---|
| MacinTalk (1984) | 3,688 bytes for a forty-character sentence, more for longer text | one `_NewPtr` in `Prime`, one `_DisposePtr` at the end |
| MacinTalk 2 | 0 | allocates everything at `Open` |
| MacinTalk 3 | 0 | likewise |
| MacinTalk Pro (`gala`) | 57,412 bytes | several `_NewPtr`/`_NewHandle`, all disposed |
| MacinTalk Pro (`cami`) | 58,200 bytes | likewise |

The 1984 driver and both Pro engines give back what they take, every
utterance, in the order a well-behaved Macintosh program does.

## What the host did with a dispose

Nothing. `_DisposePtr` and `_DisposeHandle` were served — they returned
`noErr` and cleared `MemErr` — but the heap was a bump allocator with no
notion of a free block, so the top only ever rose. Its 512 KB was gone after
**123 short utterances** of the 1984 driver, and Pro's 10 MB after **about
140**. On the next `_NewPtr` the host answered `memFullErr`, which the
engine had never been given before; the 1984 driver spun until the host's
400-million-instruction budget ended the call — **seven seconds of silence**
— and every utterance after it was silent too, until a voice switch rebuilt
the engine. A long say-all line costs more per utterance than a short
sentence, so a reader could reach it in a few dozen lines.

## Why nobody found it

Every test spoke a handful of utterances on a fresh engine, and every listener
switched voices or synthesizers long before the count. It took a fixed
*sequence* played on one engine instance — the render oracle's script,
followed by its rate × pitch × inflection grid — to speak 133 utterances in a
row, and the 133rd rendered nothing and took seven seconds. The harness was
built to catch one utterance leaking into the next; it caught the heap
leaking into the hundredth. Long-running repetition is its own class of
test, and this project did not have one before.

## The fix

`osp_host_files.c` now records every block the heap hands out. A dispose
marks its block free, and the top of the heap rolls back over every free
block at the end, so the heap returns to its post-`Open` level once an
utterance's blocks are all gone — however they were freed, in whatever
order. `_DisposeHandle` frees the block a master pointer names and then the
master pointer itself.

Deliberately, that is *all* it does. A free block in the middle of the heap
stays where it is until everything above it is free too; reusing it would
hand a later allocation an address it would not have had before, and the
engines' output is held to the byte against renders made before the fix
(`tests/baseline/renders.json` and `renders-x86.json`). With rollback only,
no block ever lands at a new address, and all 223 baseline renders on both
Windows widths and on Linux are unchanged.

Two kinds of block are pinned and never freed: a resource registered by
`osp_add_resource` and the copy of a file's resource map the File Manager
materialises for MacinTalk Pro. Both belong to the host's servers, which hand
the same Handle out again on the next request; an engine disposing of one
must not pull the memory out from under them.

A fresh block still arrives as it always did — zeroed when the *Clear*
variant was asked for, otherwise with the odd-word pattern the Spanish Pro
front end depends on — so a reused address reads exactly as a fresh one.

## Three things checked before trusting it

Because block sizes matter now where they did not before, three questions
were measured rather than assumed, on every engine, one utterance each and
then forty with the short, medium and long texts in rotation:

| engine | unserved traps in an utterance | zero-size blocks | blocks after open → after 40 | heap after 1st → after 40 |
|---|---|---|---|---|
| MacinTalk (1984) | 0 | 0 | 4 → 4 | 4,452 → 4,452 |
| MacinTalk 2 | 0 | 0 | 85 → 85 | 342,324 → 342,324 |
| MacinTalk 3 | 0 | 0 | 88 → 88 | 451,240 → 451,240 |
| Pro (`gala`) | 0 | 0 | 435 → 435 | 2,435,484 → 2,435,484 |
| Pro (`cami`) | 0 | 0 | 317 → 317 | 2,074,068 → 2,074,068 |

No engine reaches a Memory Manager trap the host does not serve
(`_SetHandleSize` is not served, and nobody asks). No allocation is ever of
size zero, which would have let two blocks share an address. And the block
table is bounded: Pro's first utterance keeps 184 blocks for the life of
the engine, then nothing more.

## What guards it now

* `src/osp_selftest.c`: `_NewPtr`, `_NewHandle`, `_DisposeHandle`,
  `_DisposePtr`, `_NewPtr` — the second pointer lands where the first did and
  the heap is back to one block. Runs in CI, no engine data.
* `tests/test_heap_reuse.py`: the 1984 engine speaks 200 utterances with a
  stable heap and identical output; Pro speaks 40. Needs the engine data.
* The render oracle's frozen baselines, which the fix had to leave untouched.
