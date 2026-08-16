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
# Deployed inside the add-on the DLL sits beside this file; in the repo it
# lives under build/. Checking both lets one module serve both places.
_CANDIDATES = [os.path.join(HERE, "osp_host.dll"),
               os.path.join(ROOT, "build", "osp_host.dll")]
DLL = next((c for c in _CANDIDATES if os.path.isfile(c)), _CANDIDATES[-1])
DLL_X86 = os.path.join(ROOT, "build", "osp_host_x86.dll")

# m68k_register_t, in declaration order
(D0, D1, D2, D3, D4, D5, D6, D7,
 A0, A1, A2, A3, A4, A5, A6, A7,
 PC, SR, SP, USP, ISP, MSP, SFC, DFC, VBR, CACR, CAAR,
 PREF_ADDR, PREF_DATA, PPC, IR) = range(31)

STOP = {0: "still running", 1: "returned to sentinel", 2: "INSTRUCTION BUDGET",
        3: "unhandled exception", 4: "fault", 5: "snapshot breakpoint"}

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
        L.osp_call_with_args.argtypes = [ctypes.c_uint,
                                         ctypes.POINTER(ctypes.c_uint),
                                         ctypes.c_int, ctypes.c_longlong]
        L.osp_pcm_get.argtypes = [ctypes.c_char_p, ctypes.c_int]
        L.osp_add_component.argtypes = [ctypes.c_uint] * 4
        L.osp_open_instance.argtypes = [ctypes.c_int]
        L.osp_open_instance.restype = ctypes.c_uint
        L.osp_instance_storage.argtypes = [ctypes.c_uint]
        L.osp_instance_storage.restype = ctypes.c_uint
        L.osp_component_call.argtypes = [ctypes.c_uint, ctypes.c_int,
                                         ctypes.POINTER(ctypes.c_uint),
                                         ctypes.c_int, ctypes.c_longlong,
                                         ctypes.POINTER(ctypes.c_uint)]
        for n in ("osp_pcm_len", "osp_sample_rate", "osp_cb_scratch"):
            getattr(L, n).restype = ctypes.c_uint
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

    def call_with_args(self, entry, args, max_instr=200_000_000):
        arr = (ctypes.c_uint * len(args))(*args)
        return self.lib.osp_call_with_args(entry, arr, len(args), max_instr)

    # --- the Component Manager --------------------------------------------
    # MacinTalk 2 and Pro are components rather than drivers, so the host also
    # answers $A82A.  See docs/macintalk2-components.md.

    @staticmethod
    def _ostype(v):
        if isinstance(v, str):
            v = v.encode("mac-roman")
        return struct.unpack(">I", v[:4].ljust(4, b" "))[0]

    def add_component(self, ctype, subtype, manuf, entry):
        """Register a component's code, as its `thng` resource would."""
        i = self.lib.osp_add_component(self._ostype(ctype),
                                       self._ostype(subtype),
                                       self._ostype(manuf), entry)
        if i < 0:
            raise RuntimeError("no room for another component")
        return i

    def open_instance(self, component):
        tok = self.lib.osp_open_instance(component)
        if not tok:
            raise RuntimeError("cannot open component %d" % component)
        return tok

    def instance_storage(self, token):
        return self.lib.osp_instance_storage(token)

    def component_call(self, token, what, args=(), max_instr=200_000_000):
        """-> (stop reason, result).  `args` is in declared order."""
        arr = (ctypes.c_uint * max(len(args), 1))(*args)
        res = ctypes.c_uint()
        r = self.lib.osp_component_call(token, what, arr, len(args),
                                        max_instr, ctypes.byref(res))
        if r < 0:
            raise RuntimeError("osp_component_call rejected the call (%d)" % r)
        return r, res.value

    @property
    def cm_log(self):
        """Every $A82A seen: (d0, pc, csp, [4 stack longs], served)."""
        out = []
        d0 = ctypes.c_uint(); pc = ctypes.c_uint(); csp = ctypes.c_uint()
        sv = ctypes.c_int(); words = (ctypes.c_uint * 4)()
        for i in range(self.lib.osp_cm_log_n()):
            self.lib.osp_cm_log_get(i, ctypes.byref(d0), ctypes.byref(pc),
                                    ctypes.byref(csp), words, ctypes.byref(sv))
            out.append((d0.value, pc.value, csp.value, list(words),
                        bool(sv.value)))
        return out

    @property
    def resource_requests(self):
        """Every (type, id, found) the engine asked the Resource Manager for.

        A voice that will not load reports only resNotFound, which does not say
        which resource was missing.  This does."""
        out = []
        t = ctypes.c_uint(); i = ctypes.c_int(); f = ctypes.c_int()
        for k in range(self.lib.osp_reslog_n()):
            self.lib.osp_reslog_get(k, ctypes.byref(t), ctypes.byref(i),
                                    ctypes.byref(f))
            out.append((struct.pack(">I", t.value).decode("mac-roman", "replace"),
                        i.value, bool(f.value)))
        return out

    @property
    def cp_wraps(self):
        return self.lib.osp_cp_wraps()

    # --- audio ------------------------------------------------------------
    def pcm_reset(self):
        self.lib.osp_pcm_reset()

    @property
    def pcm(self):
        n = self.lib.osp_pcm_len()
        buf = ctypes.create_string_buffer(n)
        self.lib.osp_pcm_get(buf, n)
        return buf.raw

    @property
    def buffers_taken(self): return self.lib.osp_buffers_taken()

    @property
    def short_buffers(self): return self.lib.osp_short_buffers()

    @property
    def sample_rate(self):
        """Hz, from the SoundHeader the driver filled in (Fixed 16.16)."""
        return self.lib.osp_sample_rate() / 65536.0

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

    # --- register snapshots at a PC -------------------------------------
    # Keyed by name so a caller can say snap["a6"] rather than count slots.
    SNAP_NAMES = ("d0 d1 d2 d3 d4 d5 d6 d7 "
                  "a0 a1 a2 a3 a4 a5 a6 a7 pc").split()

    def snap_at(self, pc, halt_on=0):
        """Record d0-d7/a0-a7/pc the first 64 times execution reaches `pc`.

        `halt_on` = N stops execution on the Nth arrival, so a probe can read
        or change emulated memory mid-run and then `resume()`.
        """
        self.lib.osp_snap_set(pc)
        self.lib.osp_snap_halt(halt_on)

    def resume(self, max_instr=400_000_000):
        """Continue from a snapshot breakpoint with every register intact."""
        self.lib.osp_resume.argtypes = [ctypes.c_longlong]
        return self.lib.osp_resume(max_instr)

    @property
    def snaps(self):
        out = []
        buf = (ctypes.c_uint * 17)()
        for i in range(self.lib.osp_snap_n()):
            self.lib.osp_snap_get(i, buf)
            out.append(dict(zip(self.SNAP_NAMES, list(buf))))
        return out

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

    def trap_d0in(self, i):
        """D0 as the *caller* set it -- the selector, for the traps that take
        one there.  `traps` reports the D0 we answered with, which for
        _Gestalt and _GetTrapAddress is the less interesting half."""
        self.lib.osp_trap_d0in.restype = ctypes.c_uint
        return self.lib.osp_trap_d0in(i)

    @property
    def stackpc_convention(self):
        v = self.lib.osp_stackpc_convention()
        return {-1: "not observed", 0: "past the trap word",
                1: "at the trap word"}[v]


def driver_entries(image):
    """(open, prime, control, status, close) from the DRVR header."""
    return struct.unpack(">HHHHH", image[8:18])
