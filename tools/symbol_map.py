# -*- coding: utf-8 -*-
"""Recover MacinTalk's routine map from its embedded MacsBug symbols.

Katz and Barton shipped `.sp` with MacsBug names left in, and the convention
places a name *after* the routine's terminating instruction.  So every name we
find is a free end-boundary, and consecutive names give us extents without
disassembling anything.

This matters more than it sounds.  21 KB of 68000 holds code, phoneme tables,
copyright strings and diad data interleaved, and a disassembler will decode all
of it as instructions without complaint.  The symbol map tells us which bytes
are actually code before we read a single mnemonic.

    py -3 tools/symbol_map.py [path-to-DRVR]
"""
import os
import re
import struct
import sys

RSRC = r"C:\git\outspoken-rsrc"
DRVR = os.path.join(RSRC, "DRVR", "1030_.sp.bin")

# The instructions a routine can end on, as big-endian words.
TERMINATORS = {
    0x4E75: "rts",
    0x4E77: "rtr",
    0x4E73: "rte",
    0x4E74: "rtd",
    # Pascal routines that pop their own arguments end on an indirect jump
    # rather than rts.  CALLBACK is one, and missing this hid six symbols.
    0x4ED0: "jmp (a0)",
    0x4ED1: "jmp (a1)",
    0x4ED2: "jmp (a2)",
    0x4ED3: "jmp (a3)",
}

NAME_CHARS = re.compile(rb"[A-Za-z0-9_%. ]+\Z")


def find_symbols(d):
    """Every MacsBug name in the image, as (name_off, name, end_of_name).

    Two encodings are in circulation and both appear in Apple-era code:

      * variable length -- a byte $80..$9F, low five bits are the length,
        then that many characters;
      * fixed eight     -- eight characters where the first has its high bit
        set, which is how the earliest tools wrote it.

    We accept either, but only when a terminator sits immediately before it.
    That anchor is what keeps table bytes from masquerading as names.
    """
    out = []
    i = 0
    while i < len(d) - 2:
        word = struct.unpack(">H", d[i:i + 2])[0]
        if word not in TERMINATORS:
            i += 2
            continue
        j = i + 2
        if j >= len(d):
            break
        b = d[j]
        name = None

        # Variable-length, two spellings.  The Pascal glue writes $80|len then
        # the name; the hand-written assembly core writes $80|len, len, then
        # the name.  Both appear in this one file -- a 1984 assembly core
        # wearing a 1988 Pascal jacket -- so accept either.
        if 0x80 <= b <= 0x9F:
            n = b & 0x1F
            if n:
                cand = d[j + 1:j + 1 + n]
                if NAME_CHARS.match(cand):
                    name, name_off, after = cand, j + 1, j + 1 + n
                elif d[j + 1] == n:                  # repeated length byte
                    cand = d[j + 2:j + 2 + n]
                    if NAME_CHARS.match(cand):
                        name, name_off, after = cand, j + 2, j + 2 + n

        # fixed-eight form: high bit set on the first character
        if name is None and 0xC1 <= b <= 0xDA:
            cand = bytes([b & 0x7F]) + d[j + 1:j + 8]
            if NAME_CHARS.match(cand):
                name, name_off, after = cand, j, j + 8

        if name is not None:
            # Pad to even, then skip MacsBug's trailing word -- it carries the
            # size of any constant data following the procedure.  Verified: the
            # next routine then starts on `link a6`, and every PC-relative jsr
            # in the sound glue resolves onto a start computed this way.
            after = after + (after & 1) + 2
            out.append((name_off, name.decode("ascii").rstrip(),
                        after, TERMINATORS[word], i))
            i = after
            continue
        i += 2
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DRVR
    d = open(path, "rb").read()
    syms = find_symbols(d)

    opn, prime, ctl, st, cls = struct.unpack(">HHHHH", d[8:18])
    entries = {opn: "Open", prime: "Prime", ctl: "Control",
               st: "Status", cls: "Close"}

    print("%s -- %d bytes, %d MacsBug symbol(s)\n"
          % (os.path.basename(path), len(d), len(syms)))

    # A routine runs from the end of the previous name to its own terminator.
    print("  %-6s %-6s %6s  %-24s %s"
          % ("start", "end", "bytes", "name", "notes"))
    prev_end = 18 + 1 + d[18]          # past the header and driver name
    prev_end += prev_end & 1
    for name_off, name, after, term, term_off in syms:
        start, end = prev_end, term_off + 2
        note = []
        if start in entries:
            note.append("<-- DRIVER %s" % entries[start])
        if term != "rts":
            note.append(term)
        print("  0x%04X 0x%04X %6d  %-24s %s"
              % (start, end, end - start, name, " ".join(note)))
        prev_end = after

    tail = len(d) - prev_end
    print("\n  0x%04X .. 0x%04X  %d bytes after the last symbol "
          "(tables / strings)" % (prev_end, len(d), tail))

    # Which driver entry points fall inside a named routine?
    print("\n  driver entry points:")
    for off, label in sorted(entries.items()):
        owner = None
        prev_end = 18 + 1 + d[18]
        prev_end += prev_end & 1
        for name_off, name, after, term, term_off in syms:
            if prev_end <= off < term_off + 2:
                owner = name
                break
            prev_end = after
        print("    %-8s +0x%04X  in %s"
              % (label, off, owner or "(no symbol -- past the named region)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
