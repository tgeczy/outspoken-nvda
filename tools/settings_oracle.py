# -*- coding: utf-8 -*-
"""Diff the host's settings arithmetic against the driver it was ported from.

`outspoken.py` maps NVDA's 0-100 sliders onto what the engines take -- a
rate curve, tenths of a semitone, hertz for the 1984 driver -- and widens
8-bit PCM to 16 with the volume folded in.  `osp_settings.c` does the same
so that SAPI, the command line and Android hear exactly what NVDA hears.
This runs both over every value of every slider, with every offset NVDA
sends, and diffs.

    py -3 tools/settings_oracle.py [path/to/osp_host.dll | libosp_host.so]

Pure arithmetic: no engine data, so it runs in CI.  Exit status is the
number of disagreements.
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers"))

import conftest                                               # noqa: E402
conftest._install_fake_nvda()
import outspoken                                              # noqa: E402


class _Recorder(object):
    """An engine that remembers what the driver asked of it."""
    def __init__(self):
        self.calls = {}

    def set_rate(self, v):        self.calls["rate"] = v
    def set_pitch(self, v):       self.calls["tenths"] = v
    def set_voice(self, v):       self.calls["hz"] = v
    def set_inflection(self, v):  self.calls["inflection"] = v


def reference(rate, radj, pitch, padj, inflection, sp_hz):
    """What outspoken.py's _applySettings would send an engine."""
    d = outspoken.SynthDriver.__new__(outspoken.SynthDriver)
    d._rate, d._pitch, d._inflection = rate, pitch, inflection
    if sp_hz:
        d._entry = lambda vid=None: ("male", "Male (MacinTalk 1)", "sp", sp_hz)
    else:
        d._entry = lambda vid=None: ("mtk2:Ben", "Ben (MacinTalk 2)", "mtk2", None)
    eng = _Recorder()
    d._applySettings(eng, padj, radj)
    return eng.calls


def load(path=None):
    if path is None:
        import osp
        path = osp.DLL
    lib = ctypes.CDLL(path)
    lib.osp_settings_preview.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                         ctypes.c_int, ctypes.c_double,
                                         ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                         ctypes.POINTER(ctypes.c_double)]
    lib.osp_pcm_widen.argtypes = [ctypes.c_char_p, ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_short), ctypes.c_int]
    lib.osp_pcm_widen.restype = ctypes.c_int
    lib.osp_set_volume.argtypes = [ctypes.c_int]
    lib.osp_set_volume_offset.argtypes = [ctypes.c_int]
    return lib


def host_preview(lib, rate, radj, pitch, padj, hz):
    er, t, h = ctypes.c_int(), ctypes.c_int(), ctypes.c_double()
    lib.osp_settings_preview(rate, radj, pitch, padj, float(hz), ctypes.byref(er),
                             ctypes.byref(t), ctypes.byref(h))
    return er.value, t.value, h.value


def run(lib, verbose=True, limit=10):
    bad = checked = 0

    def differ(what, a, b):
        nonlocal bad
        bad += 1
        if verbose and bad <= limit:
            print("DIFFER  %s\n   python: %r\n   host  : %r" % (what, a, b))

    # The rate curve and the pitch scale, every slider value with every
    # offset NVDA sends (a capital's pitch change, a RateCommand).
    for rate in range(0, 101):
        for radj in (-100, -30, -10, 0, 10, 30, 100):
            want = reference(rate, radj, 50, 0, 50, None)["rate"]
            got = host_preview(lib, rate, radj, 50, 0, 0)[0]
            checked += 1
            if want != got:
                differ("engine rate for %d%+d" % (rate, radj), want, got)
    for pitch in range(0, 101):
        for padj in (-100, -50, -30, -10, 0, 10, 30, 50, 100):
            want = reference(50, 0, pitch, padj, 50, None)["tenths"]
            got = host_preview(lib, 50, 0, pitch, padj, 0)[1]
            checked += 1
            if want != got:
                differ("tenths for %d%+d" % (pitch, padj), want, got)
            for hz in (110, 250):
                want = reference(50, 0, pitch, padj, 50, hz)["hz"]
                got = host_preview(lib, 50, 0, pitch, padj, hz)[2]
                checked += 1
                if want != got:
                    differ("hertz for %d%+d on %d Hz" % (pitch, padj, hz), want, got)

    # The widening with the volume folded in: every volume, every offset,
    # every sample value, byte for byte against the driver's tables.
    ramp = bytes(range(256))
    out = (ctypes.c_short * 256)()
    for volume in range(-5, 106):
        for vadj in (-100, -20, 0, 20, 100):
            lo, hi = outspoken.SynthDriver._gainTables(volume + vadj)
            want = bytearray(512)
            want[1::2] = ramp.translate(hi)
            if lo is not None:
                want[0::2] = ramp.translate(lo)
            lib.osp_set_volume(volume)
            lib.osp_set_volume_offset(vadj)
            n = lib.osp_pcm_widen(ramp, 256, out, 256)
            got = bytes(ctypes.string_at(ctypes.addressof(out), 512))
            checked += 1
            if n != 256 or bytes(want) != got:
                differ("widening at volume %d%+d" % (volume, vadj),
                       bytes(want)[:16].hex(), got[:16].hex())
    if verbose:
        print("%d cases, %d disagreement(s)" % (checked, bad))
    return bad


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        lib = load(path)
    except OSError as e:
        raise SystemExit("cannot load the host library (%s); run build.sh "
                         "or build_linux.sh" % e)
    return run(lib)


if __name__ == "__main__":
    sys.exit(main())
