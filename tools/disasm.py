# -*- coding: utf-8 -*-
"""Disassemble a range of MacinTalk with the A-traps resolved.

Capstone's m68k decoder is derived from Musashi -- the same core we are going
to run the driver on -- so what this prints is what the emulator will execute.

Two things a bare disassembler gets wrong on Mac code, both handled here:

  * an A-trap ($Axxx) is not an instruction, it is a Toolbox call.  Capstone
    reports it as invalid or decodes the following bytes as garbage, so we
    intercept the word ourselves and resynchronise on the next one.
  * absolute short addresses below $0400 are low-memory globals, and their
    names are the whole point.  `move.l $0266,a0` means nothing; `SoundBase`
    means we are looking at a direct-DAC sound path.

    py -3 tools/disasm.py 0x50C2 0x519A
    py -3 tools/disasm.py MACSTARTSOUND
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbol_map import find_symbols                            # noqa: E402

RSRC = r"C:\git\outspoken-rsrc"
DRVR = os.path.join(RSRC, "DRVR", "1030_.sp.bin")

# Low-memory globals.
#
# Deliberately tiny.  An earlier draft of this file carried ninety entries
# written from memory, which is a good way to label $0266 as a serial-port
# setting and never notice the sound path.  Only put an address here once
# something proves it -- this binary's own use of it, or a real reference.
#
# $0266 is earned: StuffA3 reads it as a pointer, adds $171 (369 -- one short
# of the 370-word classic sound buffer) and hands the result to the synthesiser
# as its output cursor, and it does so *only* when no Sound Manager channel
# exists.  That is the sound buffer base whatever a book calls it.
LOMEM = {
    0x0266: "SoundBase? -- sound buffer ptr, per StuffA3",
    # Read immediately after every _GetResource / _OpenResFile in DriverOpen
    # and tested for zero.  That is ResErr doing ResErr's job.
    0x0A60: "ResErr",
}

TRAPS = {
    0xA000: "_Open", 0xA001: "_Close", 0xA002: "_Read", 0xA003: "_Write",
    0xA004: "_Control", 0xA005: "_Status", 0xA006: "_KillIO",
    0xA019: "_InitZone", 0xA01F: "_DisposePtr", 0xA023: "_DisposeHandle",
    0xA024: "_SetHandleSize", 0xA025: "_GetHandleSize",
    0xA029: "_HLock", 0xA02A: "_HUnlock", 0xA02E: "_BlockMove",
    0xA033: "_VInstall", 0xA034: "_VRemove", 0xA035: "_Offline",
    0xA036: "_MoreMasters", 0xA03B: "_Delay", 0xA03C: "_CmpString",
    0xA040: "_ResrvMem", 0xA049: "_HPurge", 0xA04A: "_HNoPurge",
    0xA055: "_StripAddress", 0xA06E: "_SlotManager",
    0xA11A: "_GetZone", 0xA11E: "_NewPtr", 0xA122: "_NewHandle",
    0xA128: "_RecoverHandle", 0xA146: "_GetTrapAddress",
    0xA047: "_SetToolTrapAddress", 0xA162: "_PurgeSpace",
    0xA80D: "_Count1Resources", 0xA992: "_DetachResource",
    0xA994: "_CurResFile", 0xA998: "_UseResFile",
    0xA9A0: "_GetResource", 0xA9A1: "_GetNamedResource",
    0xA9A2: "_LoadResource", 0xA9A3: "_ReleaseResource",
    0xA9A4: "_HomeResFile", 0xA9C8: "_SysBeep", 0xA9F0: "_LoadSeg",
    0xA9EE: "_Pack6", 0xA8FE: "_InitCursor", 0xA9FF: "_Debugger",
    0xABFF: "_DebugStr",
}


def trap_name(w):
    if w in TRAPS:
        return TRAPS[w]
    stripped = (w & 0xF8FF) if (w & 0x0800) else (w & 0xF9FF)
    base = TRAPS.get(stripped)
    if base:
        flags = []
        if w & 0x0400:
            flags.append("sys" if (w & 0x0800) else "autoPop")
        if not (w & 0x0800) and (w & 0x0200):
            flags.append("immed")
        return "%s%s" % (base, (" ; " + ",".join(flags)) if flags else "")
    return "TRAP $%04X" % w


def annotate(text):
    """Name low-memory globals appearing as absolute-short operands."""
    def sub(mo):
        v = int(mo.group(1), 16)
        n = LOMEM.get(v)
        return mo.group(0) + ("{%s}" % n if n else "")
    return re.sub(r"\$(0?[0-9a-fA-F]{1,4})\b", sub, text)


def disassemble(d, start, end, base=0):
    from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN
    try:
        from capstone import CS_MODE_M68K_000
        mode = CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000
    except ImportError:                                   # older capstone
        mode = CS_MODE_BIG_ENDIAN
    md = Cs(CS_ARCH_M68K, mode)
    md.skipdata = False

    pc = start
    while pc < end:
        word = struct.unpack(">H", d[pc:pc + 2])[0]
        if 0xA000 <= word <= 0xAFFF:
            print("  +%05X  %-12s %s" % (pc, "%04X" % word, trap_name(word)))
            pc += 2
            continue
        # Read past `end` for the operand words.  Clipping the slice at the
        # requested end makes the final instruction decode from too few bytes
        # and invent an operand -- which is how a plain `move.b (a6)+,$5(a5)`
        # once read as `-$5556(a5)` and sent me hunting a relocation bug that
        # did not exist.  `end` bounds the loop, never the decoder's input.
        chunk = d[pc:pc + 16]
        insns = list(md.disasm(chunk, base + pc, count=1))
        if not insns:
            print("  +%05X  %-12s dc.w    $%04X" % (pc, "%04X" % word, word))
            pc += 2
            continue
        ins = insns[0]
        raw = "".join("%02X" % b for b in ins.bytes)
        text = annotate("%-7s %s" % (ins.mnemonic, ins.op_str))
        print("  +%05X  %-12s %s" % (pc, raw[:12], text))
        pc += ins.size


def main():
    d = open(DRVR, "rb").read()
    syms = find_symbols(d)

    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1

    if args[0].startswith("0x") or args[0].isdigit():
        start = int(args[0], 0)
        end = int(args[1], 0) if len(args) > 1 else start + 0x80
        label = "0x%04X..0x%04X" % (start, end)
    else:
        want = args[0].lower()
        prev_end = 18 + 1 + d[18]
        prev_end += prev_end & 1
        start = end = None
        for name_off, name, after, term, term_off in syms:
            if name.lower() == want:
                start, end = prev_end, term_off + 2
                break
            prev_end = after
        if start is None:
            print("no symbol %r; known: %s"
                  % (args[0], ", ".join(s[1] for s in syms)))
            return 1
        label = "%s (0x%04X..0x%04X)" % (args[0], start, end)

    print("=== %s ===" % label)
    disassemble(d, start, end)
    return 0


if __name__ == "__main__":
    sys.exit(main())
