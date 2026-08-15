# -*- coding: utf-8 -*-
"""Run MacinTalk's Open routine under Unicorn and log every Toolbox call.

We do not know what a 1984 speech driver needs from a Macintosh.  Rather than
guess, run it and let it tell us: dispatch nothing, log everything, and see
where it stops.  This is how pctalker's DOS host was built too -- five host
defects there each produced silence indistinguishable from a broken program,
and the only thing that ever settled it was an instrumented run.

    py -3 tools/trap_probe.py [path-to-DRVR]
"""
import os
import struct
import sys

import paths                                                  # noqa: E402
DRVR = paths.driver()

# --- Mac memory map -------------------------------------------------------
LOMEM = 0x0000        # low-memory globals live here
DRV_BASE = 0x00040000  # where we place the driver
HEAP = 0x00080000     # our toy heap
STACK = 0x00200000
SENTINEL = 0x00300000  # return address: PC lands here when Open returns
MEMTOP = 0x00400000

# --- the A-traps most likely to show up ----------------------------------
TRAPS = {
    0xA000: "_Open", 0xA001: "_Close", 0xA002: "_Read", 0xA003: "_Write",
    0xA004: "_Control", 0xA005: "_Status", 0xA006: "_KillIO",
    0xA00C: "_GetFileInfo", 0xA011: "_GetEOF", 0xA012: "_SetEOF",
    0xA01F: "_DisposePtr", 0xA023: "_DisposeHandle", 0xA024: "_SetHandleSize",
    0xA025: "_GetHandleSize", 0xA027: "_ReallocHandle", 0xA029: "_HLock",
    0xA02A: "_HUnlock", 0xA02E: "_BlockMove", 0xA036: "_MoreMasters",
    0xA03B: "_Delay", 0xA040: "_ResrvMem", 0xA049: "_HPurge",
    0xA04A: "_HNoPurge", 0xA055: "_StripAddress",
    0xA11E: "_NewPtr", 0xA11A: "_GetZone", 0xA122: "_NewHandle",
    0xA128: "_RecoverHandle", 0xA146: "_GetTrapAddress",
    0xA047: "_SetTrapAddress", 0xA162: "_PurgeSpace",
    0xA9A0: "_GetResource", 0xA9A1: "_GetNamedResource",
    0xA9A2: "_LoadResource", 0xA9A3: "_ReleaseResource",
    0xA992: "_DetachResource", 0xA994: "_CurResFile",
    0xA998: "_UseResFile", 0xA9A4: "_HomeResFile",
    0xA80D: "_Count1Resources", 0xA9C8: "_SysBeep",
    0xA9F0: "_LoadSeg", 0xA06E: "_SlotManager",
    0xA03C: "_CmpString", 0xA9EE: "_Pack6",
}


def trap_name(w):
    base = TRAPS.get(w)
    if base:
        return base
    # OS traps carry flag bits in 9..11; tool traps in 9..10
    if w & 0x0800:
        stripped = w & 0xF8FF
    else:
        stripped = w & 0xF9FF
    return TRAPS.get(stripped, "?") + (" (0x%04X)" % w)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DRVR
    drvr = open(path, "rb").read()
    opn, prime, ctl, status, close = struct.unpack(">HHHHH", drvr[8:18])
    print("driver %s  %d bytes" % (os.path.basename(path), len(drvr)))
    print("  Open +0x%04X  Prime +0x%04X  Control +0x%04X  "
          "Status +0x%04X  Close +0x%04X" % (opn, prime, ctl, status, close))

    sys.path.insert(0, r"C:\Python313\Lib\site-packages")
    from unicorn import (Uc, UcError, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                         UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED)
    from unicorn.m68k_const import (UC_M68K_REG_PC, UC_M68K_REG_A0,
                                    UC_M68K_REG_A1, UC_M68K_REG_A7,
                                    UC_M68K_REG_D0)

    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    uc.mem_map(0, MEMTOP)
    uc.mem_write(DRV_BASE, drvr)

    # --- a Device Control Entry, as the Device Manager would hand over ----
    dce = HEAP
    uc.mem_write(dce, b"\x00" * 40)
    uc.mem_write(dce + 0, struct.pack(">I", DRV_BASE))   # dCtlDriver
    uc.mem_write(dce + 4, struct.pack(">H", 0x4600))     # dCtlFlags
    uc.mem_write(dce + 24, struct.pack(">h", -17))       # dCtlRefNum

    # --- a parameter block ------------------------------------------------
    pb = HEAP + 0x100
    uc.mem_write(pb, b"\x00" * 80)
    uc.mem_write(pb + 24, struct.pack(">h", -17))        # ioRefNum

    state = {"traps": [], "unmapped": [], "steps": 0}

    def on_intr(uc_, intno, ud):
        pc = uc_.reg_read(UC_M68K_REG_PC)
        if intno != 10:
            state["traps"].append(("EXC%d" % intno, pc))
            print("    +0x%05X  *** 68k EXCEPTION %d *** (not an A-trap)"
                  % (pc - DRV_BASE, intno))
            state["stopped_by"] = "exception %d at driver+0x%X" % (
                intno, pc - DRV_BASE)
            uc_.emu_stop()
            return
        word = struct.unpack(">H", uc_.mem_read(pc, 2))[0]
        state["traps"].append((word, pc))
        print("    +0x%05X  %-22s D0=0x%08X A0=0x%08X"
              % (pc - DRV_BASE, trap_name(word),
                 uc_.reg_read(UC_M68K_REG_D0),
                 uc_.reg_read(UC_M68K_REG_A0)))
        # Return "no error" and a null pointer, and see how far that gets us.
        uc_.reg_write(UC_M68K_REG_D0, 0)
        uc_.reg_write(UC_M68K_REG_PC, pc + 2)

    def on_unmapped(uc_, access, addr, size, value, ud):
        state["unmapped"].append((addr, uc_.reg_read(UC_M68K_REG_PC)))
        return False

    uc.hook_add(UC_HOOK_INTR, on_intr)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    # --- call Open --------------------------------------------------------
    sp = STACK
    sp -= 4
    uc.mem_write(sp, struct.pack(">I", SENTINEL))        # return address
    uc.reg_write(UC_M68K_REG_A7, sp)
    uc.reg_write(UC_M68K_REG_A0, pb)
    uc.reg_write(UC_M68K_REG_A1, dce)

    entry = DRV_BASE + opn
    print("\ncalling Open at 0x%X (driver+0x%04X):" % (entry, opn))
    try:
        uc.emu_start(entry, SENTINEL, count=2_000_000)
        pc = uc.reg_read(UC_M68K_REG_PC)
        if state.get("stopped_by"):
            print("\n  ABORTED: %s" % state["stopped_by"])
        elif pc == SENTINEL:
            print("\n  Open RETURNED cleanly, D0 = %d"
                  % uc.reg_read(UC_M68K_REG_D0))
        else:
            print("\n  stopped early at PC 0x%X (driver+0x%X), D0 = %d"
                  % (pc, pc - DRV_BASE, uc.reg_read(UC_M68K_REG_D0)))
    except UcError as e:
        pc = uc.reg_read(UC_M68K_REG_PC)
        print("\n  stopped: %s" % e)
        print("  PC = 0x%X (driver+0x%X)" % (pc, pc - DRV_BASE))

    print("\n  %d trap(s), %d unmapped access(es)"
          % (len(state["traps"]), len(state["unmapped"])))
    for addr, pc in state["unmapped"][:8]:
        print("    unmapped 0x%08X from driver+0x%X" % (addr, pc - DRV_BASE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
