# -*- coding: utf-8 -*-
"""ctypes binding for the MacinTalk host DLL.

The DLL owns the memory and the CPU; Python owns the policy.  Traps happen a
few dozen times per utterance, so there is no reason for the trap decisions to
live in C where they are hard to change -- but the instruction loop and every
memory access stay on the C side, where they belong.

    sh build.sh          # produces build/osp_host.dll
    py -3 tools/probe_open.py
"""
import ctypes
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DLL = os.path.join(ROOT, "build", "osp_host.dll")
DLL_X86 = os.path.join(ROOT, "build", "osp_host_x86.dll")

# m68k_register_t, in declaration order
(D0, D1, D2, D3, D4, D5, D6, D7,
 A0, A1, A2, A3, A4, A5, A6, A7,
 PC, SR, SP, USP, ISP, MSP, SFC, DFC, VBR, CACR, CAAR,
 PREF_ADDR, PREF_DATA, PPC, IR) = range(31)

STOP = {0: "still running", 1: "returned to sentinel", 2: "INSTRUCTION BUDGET",
        3: "unhandled exception", 4: "fault"}

# Vector numbers worth naming when we stop on one.
VECTORS = {
    2: "bus error", 3: "address error", 4: "illegal instruction",
    5: "divide by zero", 6: "CHK", 7: "TRAPV", 8: "privilege violation",
    9: "trace", 10: "line 1010 (A-trap)", 11: "line 1111 (F-trap)",
    24: "spurious interrupt",
}


class Host(object):
    def __init__(self, ram=0x01000000, dll=None):
        path = dll or DLL
        if not os.path.isfile(path):
            raise RuntimeError("%s not built -- run `sh build.sh`" % path)
        self.lib = ctypes.CDLL(path)
        L = self.lib
        L.osp_init.argtypes = [ctypes.c_uint]
        L.osp_call.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_longlong]
        L.osp_instr_count.restype = ctypes.c_longlong
        for n in ("osp_r8", "osp_r16", "osp_r32", "osp_get_reg",
                  "osp_heap_used", "osp_magic_sentinel"):
            getattr(L, n).restype = ctypes.c_uint
        for n in ("osp_w8", "osp_w16", "osp_w32"):
            getattr(L, n).argtypes = [ctypes.c_uint, ctypes.c_uint]
        L.osp_write_block.argtypes = [ctypes.c_uint, ctypes.c_char_p, ctypes.c_int]
        L.osp_read_block.argtypes = [ctypes.c_uint, ctypes.c_char_p, ctypes.c_int]
        L.osp_set_reg.argtypes = [ctypes.c_int, ctypes.c_uint]
        L.osp_heap_init.argtypes = [ctypes.c_uint, ctypes.c_uint]
        L.osp_set_trap_policy.argtypes = [ctypes.c_uint] * 3
        L.osp_add_resource.argtypes = [ctypes.c_uint, ctypes.c_int,
                                       ctypes.c_char_p, ctypes.c_int]
        L.osp_add_resource.restype = ctypes.c_uint
        if L.osp_init(ram) != 0:
            raise RuntimeError("osp_init failed")
        self.ram = ram

    # --- memory -----------------------------------------------------------
    def load(self, addr, data):
        if self.lib.osp_write_block(addr, data, len(data)) != 0:
            raise ValueError("load outside RAM at 0x%X" % addr)

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        if self.lib.osp_read_block(addr, buf, n) != 0:
            raise ValueError("read outside RAM at 0x%X" % addr)
        return buf.raw

    def w8(self, a, v): self.lib.osp_w8(a, v)
    def w16(self, a, v): self.lib.osp_w16(a, v)
    def w32(self, a, v): self.lib.osp_w32(a, v)
    def r8(self, a): return self.lib.osp_r8(a)
    def r16(self, a): return self.lib.osp_r16(a)
    def r32(self, a): return self.lib.osp_r32(a)

    # --- cpu --------------------------------------------------------------
    def set_reg(self, r, v): self.lib.osp_set_reg(r, v)
    def get_reg(self, r): return self.lib.osp_get_reg(r)

    def heap(self, base, size):
        self.lib.osp_heap_init(base, size)

    def mem_traps(self, on=True):
        self.lib.osp_enable_mem_traps(1 if on else 0)

    def policy(self, trap_word, d0=0, a0=0):
        self.lib.osp_set_trap_policy(trap_word, d0, a0)

    def add_resource(self, restype, res_id, data):
        """Register a resource under the id the *driver* asks for.

        outSPOKEN stores these 1000 above the ids `.sp` requests -- TALK 1001
        for TALK 1 -- so the caller does the mapping, not the host.
        """
        if isinstance(restype, str):
            restype = restype.encode("mac-roman")
        t = struct.unpack(">I", restype[:4].ljust(4, b" "))[0]
        h = self.lib.osp_add_resource(t, res_id, data, len(data))
        if not h:
            raise RuntimeError("no room for resource %r %d" % (restype, res_id))
        return h

    @property
    def sentinel(self):
        return self.lib.osp_magic_sentinel()

    def call(self, entry, max_instr=20_000_000):
        return self.lib.osp_call(entry, self.sentinel, max_instr)

    # --- what happened ----------------------------------------------------
    @property
    def instr(self): return self.lib.osp_instr_count()

    @property
    def stop(self): return self.lib.osp_stop_reason()

    @property
    def stop_vector(self): return self.lib.osp_stop_vector()

    @property
    def stop_pc(self): return self.lib.osp_stop_pc()

    @property
    def stubbed(self): return self.lib.osp_stub_count()

    @property
    def faults(self):
        out = []
        addr = ctypes.c_uint(); pc = ctypes.c_uint()
        wr = ctypes.c_int(); sz = ctypes.c_int()
        for i in range(min(self.lib.osp_fault_count(), 64)):
            self.lib.osp_fault_get(i, ctypes.byref(addr), ctypes.byref(pc),
                                   ctypes.byref(wr), ctypes.byref(sz))
            out.append((addr.value, pc.value, bool(wr.value), sz.value))
        return out

    @property
    def fault_count(self): return self.lib.osp_fault_count()

    @property
    def traps(self):
        out = []
        pc = ctypes.c_uint(); w = ctypes.c_uint(); d0 = ctypes.c_uint()
        a0 = ctypes.c_uint(); a1 = ctypes.c_uint(); sv = ctypes.c_int()
        for i in range(self.lib.osp_trap_count()):
            self.lib.osp_trap_get(i, ctypes.byref(pc), ctypes.byref(w),
                                  ctypes.byref(d0), ctypes.byref(a0),
                                  ctypes.byref(a1), ctypes.byref(sv))
            out.append((pc.value, w.value, d0.value, a0.value, a1.value,
                        bool(sv.value)))
        return out

    @property
    def stackpc_convention(self):
        v = self.lib.osp_stackpc_convention()
        return {-1: "not observed", 0: "past the trap word",
                1: "at the trap word"}[v]


def driver_entries(image):
    """(open, prime, control, status, close) from the DRVR header."""
    return struct.unpack(">HHHHH", image[8:18])
