# -*- coding: utf-8 -*-
"""Run MacinTalk's DriverOpen under Musashi and report exactly what it wanted.

Unicorn could not do this -- its m68k target comes from QEMU by way of ColdFire
and rejects `movem` predecrement and `dbra`, so the driver died on the second
instruction of Open.  Musashi is MAME's core and runs real 68000.

What we expect, from docs/driver-api.md, having read Open before running it:

    _NewHandle($B00)   -> dCtlStorage at DCE+$14
    _CmpString         -> a name comparison
    _GetResource('TALK', 1)
    ...then both voice banks initialised to pitch 110/250, rate 150

Anything else is news.

    py -3 tools/probe_open.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                    # noqa: E402
from disasm import trap_name                                  # noqa: E402

import paths                                                  # noqa: E402
DRVR = paths.driver()

DRV_BASE = 0x00040000
HEAP = 0x00080000
HEAP_SIZE = 0x00080000
STACK = 0x00200000

# Low memory the driver reads.  CPUFlag is the one that matters: 0 keeps Prime
# away from `movec`, so a plain 68000 core is enough.  See docs/driver-api.md.
CPUFLAG = 0x012F
RESERR = 0x0A60


def main():
    image = open(DRVR, "rb").read()
    opn, prime, ctl, status, close = osp.driver_entries(image)
    print("driver: %d bytes" % len(image))
    print("  Open +0x%04X  Prime +0x%04X  Control +0x%04X  Status +0x%04X  "
          "Close +0x%04X\n" % (opn, prime, ctl, status, close))

    h = osp.Host()
    h.load(DRV_BASE, image)
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)          # 68000: no cache instructions anywhere
    h.w16(RESERR, 0)          # noErr

    # Open calls _GetResource('TALK', 1).  outSPOKEN stores it as TALK 1001 --
    # the +1000 offset is outSPOKEN's convention, applied here at the edge.
    talk = open(paths.talk(), "rb").read()
    h.add_resource("TALK", 1, talk)
    print("registered TALK 1 (%d bytes, from TALK 1001)\n" % len(talk))

    # A Device Control Entry, as the Device Manager would hand one over.
    dce = HEAP + HEAP_SIZE    # just past the heap, so the allocator cannot
    dce_area = 0x00190000     # collide with it
    dce = dce_area
    for off in range(0, 64, 4):
        h.w32(dce + off, 0)
    h.w32(dce + 0, DRV_BASE)      # dCtlDriver
    h.w16(dce + 4, 0x4600)        # dCtlFlags, as the header declares
    h.w16(dce + 24, 0xFFEF)       # dCtlRefNum = -17

    pb = 0x00191000               # a parameter block
    for off in range(0, 80, 4):
        h.w32(pb + off, 0)
    h.w16(pb + 24, 0xFFEF)        # ioRefNum

    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.SR, 0x2700)     # supervisor, interrupts off
    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.A0, pb)
    h.set_reg(osp.A1, dce)

    entry = DRV_BASE + opn
    print("calling Open at 0x%X (driver+0x%04X)\n" % (entry, opn))
    reason = h.call(entry, max_instr=5_000_000)

    print("  %-6s %-24s %-10s %-10s %s"
          % ("at", "trap", "D0 in", "A0 out", ""))
    for pc, word, d0, a0, a1, served in h.traps:
        print("  +%05X  %-24s 0x%08X 0x%08X %s"
              % (pc - DRV_BASE, trap_name(word), d0, a0,
                 "" if served else "<-- STUBBED, result is a guess"))

    print("\n  stop:        %s" % osp.STOP[reason], end="")
    if reason == 3:
        print("  vector %d (%s) at driver+0x%X"
              % (h.stop_vector,
                 osp.VECTORS.get(h.stop_vector, "?"),
                 h.stop_pc - DRV_BASE))
    elif reason == 1:
        print("   <-- Open RETURNED, D0 = %d" % ctypes_signed(h.get_reg(osp.D0)))
    else:
        print("  at 0x%X" % h.stop_pc)
    print("  instructions: %d" % h.instr)
    print("  traps:        %d (%d stubbed)"
          % (len(h.traps), h.stubbed))
    print("  heap used:    %d bytes" % h.lib.osp_heap_used())
    print("  faults:       %d" % h.fault_count)
    for addr, pc, wr, sz in h.faults[:8]:
        print("      %s%d at 0x%08X from driver+0x%X"
              % ("write" if wr else "read", sz * 8, addr, pc - DRV_BASE))
    print("  stacked PC:   %s" % h.stackpc_convention)

    # Did it initialise the voice banks the way the disassembly said it would?
    stor = h.r32(dce + 0x14)
    if stor:
        blk = h.r32(stor)
        print("\n  dCtlStorage handle 0x%X -> block 0x%X" % (stor, blk))
        if blk:
            fields = [("voice  $3A", 0x3A), ("rate   $32", 0x32),
                      ("pitch  $30", 0x30), ("toggle $4C", 0x4C),
                      ("v0 rate $C6", 0xC6), ("v0 pitch $C4", 0xC4),
                      ("v1 rate $CC", 0xCC), ("v1 pitch $CA", 0xCA)]
            for name, off in fields:
                print("      %-14s = %d" % (name, h.r16(blk + off)))
    return 0


def ctypes_signed(v):
    return v - 0x100000000 if v & 0x80000000 else v


if __name__ == "__main__":
    sys.exit(main())
