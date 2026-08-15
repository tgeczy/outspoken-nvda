# -*- coding: utf-8 -*-
"""Measure the frame stride instead of deducing it.

Two loaders in the synthesiser read what looks like the same three fields at
different offsets -- +$27E4 takes 8 bytes per pass, +$28DA takes 6.  A listing
cannot settle which one describes the data, because both are shipped code that
once ran.  `a6` can: snapshot it at each loader entry and the differences
between consecutive values *are* the stride, with nothing inferred.

    py -3 tools/probe_frames.py "This apple is."
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                    # noqa: E402
import probe_speak                                            # noqa: E402

DRV = probe_speak.DRV_BASE
FIRST_LOADER = 0x27E4
STEADY_LOADER = 0x28DA


def dump(h, addr, n, per=6):
    """Hex dump in `per`-byte rows, so a repeating stride shows as columns."""
    for row in range(0, n, per):
        b = [h.r8(addr + row + i) for i in range(min(per, n - row))]
        print("    +%04X  %s" % (row, " ".join("%02X" % x for x in b)))


def run(text, at):
    h, r = probe_speak.setup(text, before_prime=lambda h: h.snap_at(DRV + at))
    return h, h.snaps


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else "This apple is."

    # --- the steady-state loader: consecutive a6 give the stride ---------
    h, snaps = run(text, STEADY_LOADER)
    print("\n=== steady loader (+$%04X): %d entries ===" % (STEADY_LOADER,
                                                            len(snaps)))
    if not snaps:
        print("  never reached")
        return 1
    a6 = [s["a6"] for s in snaps]
    deltas = [b - a for a, b in zip(a6, a6[1:])]
    print("  a6: %s ..." % " ".join("0x%X" % v for v in a6[:8]))
    seen = {}
    for d in deltas:
        seen[d] = seen.get(d, 0) + 1
    print("  strides: %s" % ", ".join("%d bytes x%d" % (d, n)
                                      for d, n in sorted(seen.items())))

    # --- the first loader: where the buffer actually starts --------------
    h2, snaps2 = run(text, FIRST_LOADER)
    print("\n=== first loader (+$%04X): %d entries ===" % (FIRST_LOADER,
                                                           len(snaps2)))
    if snaps2:
        s = snaps2[0]
        base = s["a6"]
        print("  a6 = 0x%X   a5 = 0x%X" % (base, s["a5"]))
        blk = h2.r32(s["a5"] + 0x42)
        print("  $42(a5) = 0x%X   (a6 - $42(a5) = %+d)"
              % (blk, base - blk if blk else 0))
        print("\n  frame data from a6, 6-byte rows:")
        dump(h2, base, 96, 6)
        print("\n  the same bytes in 8-byte rows:")
        dump(h2, base, 96, 8)

        # Where does the first bit7-set byte fall?  Under 6-byte frames the
        # terminator sits at a6 + 6N; under 8-byte frames at a6 + 8N.
        for i in range(0, 4096):
            if h2.r8(base + i) & 0x80:
                print("\n  first byte with bit 7 set: +%d  (%%6 = %d, %%8 = %d)"
                      % (i, i % 6, i % 8))
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
