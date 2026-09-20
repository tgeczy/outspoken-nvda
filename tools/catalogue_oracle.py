# -*- coding: utf-8 -*-
"""Diff the host's voice catalogue against the Python it was ported from.

`voices.py`, the engine modules' `find()` and `outspoken.py`'s `_catalogue`
are the reference: which voices can speak, in what order, under which ids
and labels, and the manifest that opens each one's engine.  `osp_voices.c`
answers the same questions from C so that the command line, the SAPI host
and Android need no Python to list a voice.  This asks both and diffs.

    py -3 tools/catalogue_oracle.py [path/to/osp_host.dll | libosp_host.so]

Exit status is the number of disagreements.  Needs the engine data under
the usual roots (`$OUTSPOKEN_ROM`, `rom/`), so it runs here and on the
Linux box rather than in CI.
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))

import paths                                                  # noqa: E402
import voices as voicelib                                     # noqa: E402
import render_oracle                                          # noqa: E402

GENDER_CODE = dict((v, k) for k, v in voicelib.GENDER.items())


def load(path=None):
    if path is None:
        import osp
        path = osp.DLL
    lib = ctypes.CDLL(path)
    lib.osp_catalogue_scan.argtypes = [ctypes.c_char_p]
    lib.osp_catalogue_scan.restype = ctypes.c_int
    lib.osp_catalogue_count.restype = ctypes.c_int
    lib.osp_catalogue_skipped_count.restype = ctypes.c_int
    for name in ("osp_catalogue_entry", "osp_catalogue_manifest", "osp_catalogue_skipped"):
        f = getattr(lib, name)
        f.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        f.restype = ctypes.c_int
    return lib


def _text(fn, i):
    cap = 4096
    for _ in range(3):
        buf = ctypes.create_string_buffer(cap)
        n = fn(i, buf, cap)
        if n < 0:
            raise RuntimeError("catalogue call failed: %d" % n)
        if n < cap:
            return buf.raw[:n].decode("utf-8")
        cap = n + 1
    raise RuntimeError("catalogue kept asking for a bigger buffer")


def host_catalogue(lib, roots):
    n = lib.osp_catalogue_scan("\n".join(roots).encode("utf-8"))
    if n < 0:
        raise RuntimeError("osp_catalogue_scan failed")
    entries = [_text(lib.osp_catalogue_entry, i).split("\t") for i in range(n)]
    manifests = [_text(lib.osp_catalogue_manifest, i) for i in range(n)]
    skipped = [tuple(_text(lib.osp_catalogue_skipped, i).split("\t", 1))
               for i in range(lib.osp_catalogue_skipped_count())]
    return entries, manifests, skipped


def _gender(v):
    g = v.gender
    if g in GENDER_CODE:
        return GENDER_CODE[g]
    return int(g.split()[-1])


def python_catalogue(roots):
    """(entries, manifests, skipped) as the Python modules answer them."""
    entries, manifests, skipped = [], [], []
    refs = dict((r.label, r) for r in render_oracle.engine_refs())

    if "sp" in refs:
        ref = refs["sp"]
        for vid, label, hz, gender in (("male", "Male", 110, 1), ("female", "Female", 250, 2)):
            entries.append([vid, "%s (MacinTalk 1)" % label, "sp", "sp", str(hz),
                            label, "en", str(gender), ""])
            manifests.append(ref.manifest([v for v in ref.voices if v.hz == hz][0]))

    import macintalk2
    import macintalk3
    import macintalkpro

    def add(kind, creator, engine_label, ref):
        for v in found:
            entries.append(["%s:%s" % (creator if kind != "mtk2" and kind != "mtk3" else kind, v.name),
                            "%s (%s)" % (v.name, engine_label), kind, v.creator, str(v.id),
                            v.name, "es" if v.creator == "cami" else "en", str(_gender(v)),
                            v.folder])
            manifests.append(ref.manifest([x for x in ref.voices if x.creator == v.creator and x.id == v.id][0]))

    f2, _v2 = macintalk2.find(roots)
    if f2 and all(r in f2 for r in macintalk2.REQUIRED):
        found, bad = voicelib.installed("mtk2", roots=roots, speakable=True)
        skipped += bad
        if found:
            add("mtk2", "mtk2", "MacinTalk 2", refs["mtk2"])

    if macintalk3.engine_dir(roots):
        found, bad = voicelib.installed("mtk3", roots=roots, speakable=True)
        skipped += bad
        if found:
            add("mtk3", "mtk3", "MacinTalk 3", refs["mtk3"])

    for creator in ("gala", "cami"):
        if not macintalkpro.engine_dir(roots, creator):
            continue
        found, bad = voicelib.installed(creator, roots=roots, speakable=True)
        skipped += bad
        for v in found:
            entries.append(["%s:%s" % (v.creator, v.name), "%s (MacinTalk Pro)" % v.name,
                            "gala", v.creator, str(v.id), v.name,
                            "es" if v.creator == "cami" else "en", str(_gender(v)), v.folder])
            ref = [r for r in render_oracle.engine_refs()
                   if r.kind == "pro" and r.voices[0].creator == v.creator and r.voices[0].id == v.id][0]
            manifests.append(ref.manifest(ref.voices[0]))
    return entries, manifests, skipped


def run(lib, verbose=True, limit=10):
    roots = paths.roots()
    want_e, want_m, want_s = python_catalogue(roots)
    got_e, got_m, got_s = host_catalogue(lib, roots)
    bad = 0

    def differ(what, a, b):
        nonlocal bad
        bad += 1
        if verbose and bad <= limit:
            print("DIFFER  %s" % what)
            print("   python: %r" % (a,))
            print("   host  : %r" % (b,))

    if len(want_e) != len(got_e):
        differ("entry count", len(want_e), len(got_e))
    for i in range(min(len(want_e), len(got_e))):
        if want_e[i] != got_e[i]:
            differ("entry %d" % i, want_e[i], got_e[i])
        if want_m[i] != got_m[i]:
            wl, gl = want_m[i].split("\n"), got_m[i].split("\n")
            first = next((k for k in range(max(len(wl), len(gl)))
                          if k >= len(wl) or k >= len(gl) or wl[k] != gl[k]), None)
            differ("manifest %d (%s), line %s" % (i, want_e[i][0], first),
                   wl[first] if first is not None and first < len(wl) else None,
                   gl[first] if first is not None and first < len(gl) else None)
    if [tuple(s) for s in want_s] != [tuple(s) for s in got_s]:
        differ("skipped folders", want_s, got_s)
    if verbose:
        print("%d entries, %d skipped folders; %d disagreement(s)"
              % (len(want_e), len(want_s), bad))
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
