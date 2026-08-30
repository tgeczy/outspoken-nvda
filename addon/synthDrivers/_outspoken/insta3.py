# -*- coding: utf-8 -*-
"""InstaCompOne's codec, with the EXACT high-branch width cascades.

InstaCompOne is the LZ77 + Huffman codec Apple's Installer 4.0.3 used inside a
`.tome` archive, and it is the last of the four wrappers between a downloaded
`.smi.bin` and the speech engine inside it (see `smi.py` and
`docs/self-mounting-images.md`). It is *not* the Resource Manager's `dcmp` --
but the Installer's own `dcmp 3` implements the same bitstream, and every value
here was read from a disassembly of `dcmp_3.bin` and confirmed against a live
68k run and Apple's own known-plaintext Tome oracle.

The one part that cannot be written as a formula is the distance high-branch bit
width: it is a per-tier cascade with anomalies -- tier 7's width-10 boundary is
1644, not the 1664 a clean `21*2^n` ladder would give. The tables below are
those cascades, extracted verbatim. `decode_length`, `decode_literal` and the
small/mid distance branches are plain and were checked against Apple plaintext.
"""

#: Magnitude ladder: the tier is the first row whose threshold the running
#: output position does not exceed. The two topmost (12 = 70000, 13 = 172032)
#: are literal values, not `21*2^n`; 86016 was an early guess and is wrong.
THRESH = [(10, 0), (20, 1), (40, 2), (80, 3), (160, 4),
          (672, 5), (1000, 6), (2688, 7), (5376, 8), (10752, 9),
          (21504, 10), (43008, 11), (70000, 12), (172032, 13),
          (float("inf"), 14)]

#: HIGH_CASCADE[n] = (guards [(threshold, width)...], else_width). The width is
#: the first guard whose threshold the magnitude does not exceed, else the
#: else_width. Tiers below 5 use the plain `ceil_log2(mag - 5*2^n)`.
HIGH_CASCADE = {
    5: ([(162, 1), (164, 2), (168, 3), (176, 4), (192, 5), (224, 6), (288, 7), (416, 8)], 9),
    6: ([(322, 1), (324, 2), (328, 3), (336, 4), (352, 5), (384, 6), (448, 7), (576, 8), (832, 9)], 10),
    7: ([(642, 1), (644, 2), (648, 3), (656, 4), (672, 5), (704, 6), (768, 7), (896, 8), (1152, 9), (1644, 10)], 11),
    8: ([(1282, 1), (1284, 2), (1288, 3), (1296, 4), (1312, 5), (1344, 6), (1408, 7), (1536, 8), (1792, 9), (2304, 10), (3328, 11)], 12),
    9: ([(2562, 1), (2564, 2), (2568, 3), (2576, 4), (2592, 5), (2624, 6), (2688, 7), (2816, 8), (3072, 9), (3584, 10), (4608, 11), (6656, 12)], 13),
    10: ([(5122, 1), (5124, 2), (5128, 3), (5136, 4), (5152, 5), (5184, 6), (5248, 7), (5376, 8), (5632, 9), (6144, 10), (7168, 11), (9216, 12), (13312, 13)], 14),
    11: ([(10242, 1), (10244, 2), (10248, 3), (10256, 4), (10272, 5), (10304, 6), (10368, 7), (10496, 8), (10752, 9), (11264, 10), (12288, 11), (14336, 12), (18432, 13), (26624, 14)], 15),
    12: ([(20482, 1), (20484, 2), (20488, 3), (20496, 4), (20512, 5), (20544, 6), (20608, 7), (20736, 8), (20992, 9), (21504, 10), (22528, 11), (24576, 12), (28672, 13), (36864, 14), (53248, 15)], 16),
    13: ([(40962, 1), (40964, 2), (40968, 3), (40976, 4), (40992, 5), (41024, 6), (41088, 7), (41216, 8), (41472, 9), (41984, 10), (43008, 11), (45056, 12), (49152, 13), (57344, 14), (73728, 15), (106496, 16)], 17),
    14: ([(81922, 1), (81924, 2), (81928, 3), (81936, 4), (81952, 5), (81984, 6), (82048, 7), (82176, 8), (82432, 9), (82944, 10), (83968, 11), (86016, 12), (90112, 13), (98304, 14), (114688, 15), (147456, 16), (212992, 17)], 18),
}


class Ctx(object):
    """A big-endian, MSB-first bit reader over `buf` starting at `pos`."""
    __slots__ = ("buf", "pos", "acc", "bits")

    def __init__(self, buf, pos):
        self.buf = buf
        self.pos = pos
        self.acc = 0
        self.bits = 0

    def getbits(self, count):
        # A loop-refilling reader, value-identical to dcmp 3's two readers.
        while self.bits < count:
            self.acc = ((self.acc << 8) | self.buf[self.pos]) & 0xFFFFFFFFFFFFFFFF
            self.pos += 1
            self.bits += 8
        self.bits -= count
        return (self.acc >> self.bits) & ((1 << count) - 1 if count else 0)

    reader2 = getbits


def decode_length(c):
    u = 0
    while u < 10 and c.getbits(1):
        u += 1
    if u == 0:
        return c.getbits(1)
    if u == 1:
        return 2 if c.getbits(1) == 0 else c.getbits(1) + 3
    if u == 2:
        return c.getbits(1) + 5 if c.getbits(1) == 0 else c.getbits(2) + 7
    bits, base = {3: (3, 11), 4: (3, 19), 5: (5, 27), 6: (6, 59),
                  7: (7, 123), 8: (8, 251), 9: (9, 507), 10: (10, 1019)}[u]
    return c.getbits(bits) + base


def decode_literal(c):
    if c.getbits(1) == 0:
        return 1
    u = c.getbits(2)
    if u == 0:
        return 2
    if u == 1:
        return 3
    if u == 2:
        return c.getbits(2) + 4
    d = c.getbits(4)
    if d <= 7:
        return d + 8
    if d <= 11:
        return (d - 8) * 4 + 16 + c.getbits(2)
    return (d - 12) * 8 + 32 + c.getbits(3)


def _tier(mag):
    for thr, n in THRESH:
        if mag <= thr:
            return n


def _ceil_log2(x):
    return 1 if x < 2 else (x - 1).bit_length()


def _high_width(n, mag):
    if n not in HIGH_CASCADE:                # small tiers: the plain formula
        return _ceil_log2(mag - 5 * (1 << n))
    guards, else_w = HIGH_CASCADE[n]
    for thr, w in guards:
        if mag <= thr:
            return w
    return else_w


def decode_distance(c, mag):
    n = _tier(mag)
    small_max = 1 << n
    if c.getbits(1):
        if c.getbits(1) == 0:
            return c.getbits(n + 2) + small_max + 1                 # mid
        w = _high_width(n, mag)                                     # EXACT cascade
        return c.getbits(w) + 5 * small_max + 1                     # high
    return c.getbits(n) + 1 if n else 1                             # small
