# -*- coding: utf-8 -*-
"""Map MacinTalk Pro's module pipeline, and read any of it out of live memory.

**Every module address in the notes is only true of the run that produced it.**
Pro's modules are loaded lazily into the heap during `SpeakBuffer`, so where
they land depends on allocation order, which depends on the voice. Agnes and
Bruce put the same module at different addresses. An hour was lost to reading
zeros out of an address that was correct for a different run.

The one landmark that survives is inside `gtse 1`, which loads at a fixed
`0x40000`:

    0x4167C  the dispatch function -- (globals, node) -> status in d0
    0x416B2    pea.l   $3e(a3)      ; the request
    0x416B6    movea.l $4(a3), a0   ; the module's entry point
    0x416BA    jsr     (a0)

So snap `0x416BA` and let the engine tell you where its modules are.

    py -3 tools/probe_pro_modules.py                 # the dispatch map
    py -3 tools/probe_pro_modules.py Bruce           # for another voice
    py -3 tools/probe_pro_modules.py -d 0x1BCE10:110 # disassemble live memory
    py -3 tools/probe_pro_modules.py -d mod0+0x29C:110

`mod0+OFF` resolves against this run's own module list, which is the only form
worth writing down anywhere.

## What the pipeline does, measured

Seven modules, twenty-one dispatches, and the message is `node+$48`:

    0 = init    all seven, in order
    1 = prime   five of them
    3 = pump    the text module, six times
    2 = flush   the text module twice, then module #1, and then it stops

The scheduler carries on only while the dispatch returns **0** in `d0`
(`0x43CDC`: `jsr $4167c / beq` -- anything else bails out). The byte at
`request+$0B` is a separate thing: the module's *state*, which the scheduler
presets to 8 and the module overwrites. Do not confuse the two; the notes did
for a while.

`$10` and `$14` of the request are a producer/consumer pipe -- bytes available
and bytes consumed -- and `$c` is the buffer.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
from disasm import disassemble                                 # noqa: E402
from probe_pro_open import (TEXT_BUF, PARAM_BUF, VOICE_SPEC,    # noqa: E402
                            build, signed)

SPEAK, SET_INFO = 1, 6
SO_CURRENT_VOICE = 0x63766F78                                  # 'cvox'
SO_RATE = 0x72617465                                           # 'rate'
DISPATCH_CALL = 0x416BA
TEXT = b"Hello there."

#: What the message byte means, read off the run rather than guessed.
MESSAGES = {0: "init", 1: "prime", 2: "flush", 3: "pump"}


def fixed(x):
    return int(round(x * 65536.0)) & 0xFFFFFFFF


def speak(want=None, text=TEXT):
    """Open, choose the voice, set the rate, speak. Returns the host."""
    h, tok, voice, (reason, result) = build(want)
    if reason != 1 or result != 0:
        raise SystemExit("Open failed -- run tools/probe_pro_open.py")
    h.snap_at(DISPATCH_CALL)
    creator = voice.creator.encode("mac-roman", "replace")
    h.w32(VOICE_SPEC, int.from_bytes(creator[:4].ljust(4, b" "), "big"))
    h.w32(VOICE_SPEC + 4, voice.id)
    h.component_call(tok, SET_INFO, [SO_CURRENT_VOICE, VOICE_SPEC])
    h.w32(PARAM_BUF, fixed(180))
    h.component_call(tok, SET_INFO, [SO_RATE, PARAM_BUF])
    h.load(TEXT_BUF, text + b"\0")
    h.component_call(tok, SPEAK, [TEXT_BUF, len(text), 0])
    return h, voice


def modules(h):
    """This run's module entry points, in the order the engine first uses
    them. `a0` is the entry and `a3` the scheduler node, both captured live,
    so neither can be stale."""
    order, seen = [], {}
    for s in h.snaps:
        if s["a0"] not in seen:
            seen[s["a0"]] = len(seen)
            order.append((s["a0"], s["a3"]))
    return order, seen


def main():
    args = [a for a in sys.argv[1:]]
    dump = None
    if "-d" in args:
        i = args.index("-d")
        dump = args[i + 1]
        del args[i:i + 2]
    want = args[0] if args else None

    h, voice = speak(want)
    order, seen = modules(h)

    print("\nvoice %s -- %d dispatches at 0x%X\n"
          % (voice.name, len(h.snaps), DISPATCH_CALL))
    for i, s in enumerate(h.snaps, 1):
        node = s["a3"]
        # Read the message live only for the last dispatch to each node: the
        # byte is overwritten, so an earlier one would be a stale reading --
        # the trap that produced a wrong landmark in the notes.
        print("  %2d  module #%d  entry 0x%06X  node 0x%06X"
              % (i, seen[s["a0"]], s["a0"], node))

    print("\n%d modules, and their final state:" % len(order))
    for entry, node in order:
        msg = h.r8(node + 0x48)
        print("  #%d  entry 0x%06X  node 0x%06X  last message %d (%s), "
              "state %d" % (seen[entry], entry, node, msg,
                            MESSAGES.get(msg, "?"), h.r8(node + 0x49)))

    if dump:
        where, _, length = dump.partition(":")
        length = int(length) if length else 128
        if where.startswith("mod"):
            which, _, off = where[3:].partition("+")
            addr = order[int(which)][0] + int(off, 16)
        else:
            addr = int(where, 16)
        data = h.read(addr, length)
        if not any(data):
            print("\n0x%06X: all zeros -- not loaded, or the wrong run" % addr)
        else:
            print("\n=== 0x%06X, %d bytes, live" % (addr, length))
            disassemble(data, 0, length, base=addr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
