# -*- coding: utf-8 -*-
"""Open MacinTalk 2's front end as the Component Manager would, and report.

Milestone 1 of docs/macintalk2-components.md.  `probe_open.py` is the same
shape for `.sp`; the difference is that a component is not a DRVR, so the host
plays Component Manager as well as Speech Manager.

What the disassembly says should happen, read before running it:

    selector -1 (Open) -> Cecy 3 +$118
      $A82A D0=$10   GetComponentInstanceStorage(self)  -> nil, so carry on
      $A82A D0=$0E   -- picks an allocation strategy; either answer is fine
      _NewHandle($21C) then _HLock
      $A82A D0=$11   SetComponentInstanceStorage(self, that handle)
      _NewHandle($322), _MoveHHi

Anything else is news, and the $A82A log below is the point of the exercise:
an unknown selector halts rather than being stubbed, with its stack intact.

    py -3 tools/probe_mt2_open.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
import paths                                                   # noqa: E402
from disasm import trap_name                                   # noqa: E402

FRONT_BASE = 0x00040000
BACK_BASE = 0x00060000
HEAP = 0x00080000
HEAP_SIZE = 0x00080000
STACK = 0x00200000

MEMERR = 0x0220
RESERR = 0x0A60
CPUFLAG = 0x012F

#: The standard component selectors, per the -1..-6 table at Cecy 3 +$30.
OPEN, CLOSE, CANDO, VERSION, REGISTER, TARGET = -1, -2, -3, -4, -5, -6

#: What the front end needs registered.  IDs are the extractor's; the trap log
#: says whether the engine agrees, exactly as it did for `.sp`'s TALK 1 / 1001.
TABLES = [("ttsr", 1), ("ttsd", 1), ("ttsd", 2), ("ttss", 0),
          ("ttph", 1), ("ttop", 1)]

CM_NAMES = {0xFFFFFFFF: "CallComponentFunctionWithStorage",
            0x00000000: "call component instance",
            0x0000000E: "(allocation strategy)",
            0x00000010: "GetComponentInstanceStorage",
            0x00000011: "SetComponentInstanceStorage"}


def rom(name):
    p = paths.find(name)
    if not p:
        raise SystemExit(
            "cannot find %s.\n"
            "Fill rom/macintalk2 from your own disk image:\n"
            "    py -3 tools/extract_rom.py \"C:/path/to/MacOS7.hfv\"" % name)
    return p


def signed(v):
    return v - 0x100000000 if v & 0x80000000 else v


def main():
    front = open(rom("Cecy_3.bin"), "rb").read()
    back = open(rom("Cecy_1.bin"), "rb").read()
    print("front end  Cecy 3  %6d bytes at 0x%X" % (len(front), FRONT_BASE))
    print("back end   Cecy 1  %6d bytes at 0x%X\n" % (len(back), BACK_BASE))

    h = osp.Host()
    h.load(FRONT_BASE, front)
    h.load(BACK_BASE, back)
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)
    h.w16(RESERR, 0)
    h.w16(MEMERR, 0)

    for rtype, rid in TABLES:
        path = paths.find("%s_%d.bin" % (rtype, rid))
        if not path:
            print("  (no %s %d -- carrying on, the trap log will say if it "
                  "was wanted)" % (rtype, rid))
            continue
        data = open(path, "rb").read()
        h.add_resource(rtype, rid, data)
        print("  registered %s %d (%d bytes)" % (rtype, rid, len(data)))

    # The two thng resources say what these are: Cecy 1 is 't2be'/'t2be'/'mtk2'
    # and Cecy 3 is 'ttsc'/'mtk2'/'mtk2'.
    fe = h.add_component("ttsc", "mtk2", "mtk2", FRONT_BASE)
    be = h.add_component("t2be", "t2be", "mtk2", BACK_BASE)
    self_tok = h.open_instance(fe)
    print("\nfront end is component %d, instance 0x%08X" % (fe, self_tok))
    print("back end  is component %d (not opened yet -- the front end "
          "should ask)\n" % be)

    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.SR, 0x2700)

    reason, result = h.component_call(self_tok, OPEN, [self_tok],
                                      max_instr=20_000_000)

    print("$A82A calls, in order:")
    if not h.cm_log:
        print("  (none -- the front end never reached its own glue)")
    for d0, pc, csp, words, served in h.cm_log:
        print("  +%05X  D0=0x%08X  %-34s %s"
              % (pc - FRONT_BASE, d0, CM_NAMES.get(d0, "*** UNKNOWN ***"),
                 "" if served else "<-- HALTED HERE"))
        if not served:
            print("           stack at 0x%08X: %s"
                  % (csp, " ".join("0x%08X" % w for w in words)))

    print("\nToolbox and OS traps:")
    for pc, word, d0, a0, a1, served in h.traps:
        where = ("front+0x%05X" % (pc - FRONT_BASE) if pc < BACK_BASE
                 else "back+0x%05X" % (pc - BACK_BASE))
        print("  %-16s %-22s D0=0x%08X A0=0x%08X %s"
              % (where, trap_name(word), d0, a0,
                 "" if served else "<-- STUBBED"))

    print("\n  stop:          %s" % osp.STOP[reason], end="")
    if reason == 3:
        print("   vector %d (%s) at 0x%X"
              % (h.stop_vector, osp.VECTORS.get(h.stop_vector, "?"),
                 h.stop_pc))
    elif reason == 1:
        print("   <-- Open RETURNED, result = %d" % signed(result))
    else:
        print("   at 0x%X" % h.stop_pc)
    print("  instructions:  %d" % h.instr)
    print("  traps:         %d (%d stubbed)" % (len(h.traps), h.stubbed))
    print("  $A82A calls:   %d" % len(h.cm_log))
    print("  heap used:     %d bytes" % h.lib.osp_heap_used())
    print("  MemErr:        %d" % signed(h.r16(MEMERR)))
    print("  faults:        %d" % h.fault_count)
    for addr, pc, wr, sz in h.faults[:8]:
        print("      %s%d at 0x%08X from 0x%X"
              % ("write" if wr else "read", sz * 8, addr, pc))

    stor = h.instance_storage(self_tok)
    print("\n  storage handle 0x%X" % stor)
    if stor:
        blk = h.r32(stor)
        print("  storage block  0x%X" % blk)
        if blk:
            print("      +$21A (allocation flag) = %d" % h.r16(blk + 0x21A))
            print("      +$004 (back-end instance) = 0x%08X" % h.r32(blk + 4))
    return 0 if reason == 1 else 1


if __name__ == "__main__":
    sys.exit(main())
