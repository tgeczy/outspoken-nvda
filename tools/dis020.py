# -*- coding: utf-8 -*-
"""Disassemble MacinTalk Pro's 68020 code, from the resource files themselves.

`tools/disasm.py` is 68000-only, and Pro is 68020: `4C00`/`4C01` long multiplies
and `EBE8` bitfield ops come out as `dc.w` and **desynchronise the listing after
them**, which is how two wrong landmarks got into the notes. This uses capstone
in `CS_MODE_M68K_020`.

It reads the extracted resource rather than emulated memory, so a listing is
reproducible and does not depend on where a module happened to land:

    py -3 tools/dis020.py gtse_1 0x4167C 0x50        # gtse 1 is fixed at 0x40000
    py -3 tools/dis020.py gtse_2 +0x3044 0x80        # module-relative
    py -3 tools/dis020.py gtse_2 +0x3044 0x80 --run  # ...and this run's addresses

`gtse 1` always loads at `0x40000`. Everything else is loaded lazily into the
heap during `SpeakBuffer` and moves with the voice, so a module is addressed
`+OFFSET` from its own start; `--run` adds the live base for the current Agnes
run so a listing can be compared with a halt.

Which resource is which module, measured by matching the loaded bytes back to
the files (`resources.tsv` supplies the names):

    #0 gtse 7  *Cmd       #1 gtse 2  EnglPhon   #2 gtse 8  *XPh
    #3 gtse 4  *Wave      #4 gtse 9  *XAl       #5 gtse 3  EnglAllo
    #6 gtse 5  *Snd

`gtse 6` (`*Lex`) is loaded too but is **not** one of the seven scheduler nodes:
EnglPhon calls it as a service, and it is where the asynchronous lexicon read
lives.
"""
import os
import sys

import struct

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import trap_name                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "rom", "macintalkpro")

#: resource -> (module number, name).  `gtse 1` is the scheduler itself.
MODULES = {
    "gtse_1": (None, "*TTS (scheduler)"), "gtse_7": (0, "*Cmd"),
    "gtse_2": (1, "EnglPhon"), "gtse_8": (2, "*XPh"), "gtse_4": (3, "*Wave"),
    "gtse_9": (4, "*XAl"), "gtse_3": (5, "EnglAllo"), "gtse_5": (6, "*Snd"),
    "gtse_6": (None, "*Lex (a service, not a node)"),
}

#: where each landed in the reference Agnes run -- for comparing with a halt.
AGNES_BASE = {"gtse_1": 0x40000, "gtse_7": 0x1BCB74, "gtse_2": 0x1BFA78,
              "gtse_8": 0x1D0DA8, "gtse_4": 0x1E33F4, "gtse_9": 0x1D2CF8,
              "gtse_3": 0x1D4688, "gtse_5": 0x1E1DDC, "gtse_6": 0x1C76B8}


def md():
    m = capstone.Cs(capstone.CS_ARCH_M68K,
                    capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_020)
    m.detail = False
    return m


def listing(name, off, length, base=0):
    """Disassemble `length` bytes from file offset `off`. `base` is added to
    every address, so pass the live load address to compare with a halt.

    **capstone stops dead at an A-line trap**, which is half of what Pro's code
    is made of, so decoding is resumed a word at a time past anything it
    refuses. Without this the listing simply ends at the first `_NewHandle` and
    looks like the end of the function -- the same class of silent truncation
    that put two wrong landmarks in the notes.
    """
    data = open(os.path.join(ROM, name + ".bin"), "rb").read()
    dis, lines, pos = md(), [], 0
    while pos < length:
        moved = False
        for i in dis.disasm(data[off + pos:off + length], base + off + pos):
            lines.append("%06X  %-20s %s %s"
                         % (i.address, i.bytes.hex(" "), i.mnemonic, i.op_str))
            pos += i.size
            moved = True
        if pos >= length:
            break
        w = struct.unpack_from(">H", data, off + pos)[0]
        name_ = trap_name(w) if 0xA000 <= w <= 0xAFFF else None
        lines.append("%06X  %-20s dc.w $%04X%s"
                     % (base + off + pos, data[off + pos:off + pos + 2].hex(" "),
                        w, "        ; %s" % name_ if name_ else ""))
        pos += 2
        if not moved and pos >= length:
            break
    return lines


def main():
    args = sys.argv[1:]
    run = "--run" in args
    args = [a for a in args if a != "--run"]
    if len(args) < 2:
        raise SystemExit(__doc__)
    name = args[0].replace(".bin", "")
    where = args[1]
    length = int(args[2], 0) if len(args) > 2 else 0x60

    fixed_base = 0x40000 if name == "gtse_1" else 0
    if where.startswith("+"):
        off = int(where[1:], 0)
    else:                                  # an absolute address from a halt
        addr = int(where, 0)
        off = addr - AGNES_BASE.get(name, 0)
    base = AGNES_BASE[name] if (run or name == "gtse_1") else fixed_base

    num, label = MODULES.get(name, (None, "?"))
    print("%s = %s%s, %s+0x%X%s"
          % (name.replace("_", " "), label,
             "" if num is None else "  (module #%d)" % num,
             name, off, ", live at 0x%06X" % (base + off) if base else ""))
    for line in listing(name, off, length, base):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
