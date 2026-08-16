# -*- coding: utf-8 -*-
"""Build the `.nvda-addon`, and refuse to build one that ships an engine.

An NVDA add-on is a zip with a different extension, so this is short. The part
worth having is the refusal: **nothing in MacinTalk, outSPOKEN or any voice may
leave this machine.** Those belong to Katz and Barton, to Berkeley Systems and
to Apple; the user supplies them from their own copy, and the whole arrangement
fails the first time one of them lands in a release.

`.gitignore` protects the repository. This protects the thing people actually
download, which git never sees.

Both binaries go in. NVDA was a 32-bit process for most of its life, and an
add-on carrying only the 64-bit build fails on the rest with
"[WinError 193] %1 is not a valid Win32 application" -- reported by a user as
"it loads but there is no speech", because the synthesizer appeared in the list
and only failed once it was selected.

    sh build.sh                 # produces both DLLs
    py -3 tools/package.py      # produces outspoken-<version>.nvda-addon
"""
import os
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(ROOT, "addon")
BUILD = os.path.join(ROOT, "build")

#: The two binaries the add-on cannot run without, and where each belongs.
DLLS = [("osp_host.dll", "synthDrivers/_outspoken/osp_host.dll", 64),
        ("osp_host_x86.dll", "synthDrivers/_outspoken/osp_host_x86.dll", 32)]

#: Anything matching these must never be packaged. Extensions catch the bulk;
#: the names catch a file someone renamed without thinking.
FORBIDDEN_EXT = (".bin", ".rsrc", ".hfv", ".dsk", ".img", ".wav", ".sit")
FORBIDDEN_NAME = ("drvr", "talk", "rulz", "dict", "phnm", "cecy", "ttv",
                  "ttsr", "ttsd", "ttss", "gtse", "gtss", "outspoken.bin",
                  "cmudict")

SKIP_DIRS = {"__pycache__", ".git", "rom"}


def forbidden(rel, name):
    low = name.lower()
    if low.endswith(FORBIDDEN_EXT) and not low.endswith(".dll"):
        return "%s looks like extracted engine data" % rel
    stem = os.path.splitext(low)[0]
    for bad in FORBIDDEN_NAME:
        if stem.startswith(bad):
            return "%s is named like an engine resource" % rel
    return None


#: The oldest Windows the add-on claims to run on. A PE declaring anything
#: newer is refused *by Windows itself*, before a single import is looked at.
MIN_OS = (6, 1)                      # Windows 7

#: Everything the DLLs are allowed to need. Kept tiny on purpose: `/MT` links
#: the C runtime statically, so a `vcruntime140.dll` or an `api-ms-win-crt-*`
#: appearing here means the build switched to `/MD` and would fail on any
#: machine without the exact Visual C++ redistributable installed.
ALLOWED_IMPORTS = {"kernel32.dll"}


def pe_info(path):
    """Bitness, minimum OS version and imported libraries, from the headers.

    Deliberately does not shell out to dumpbin: the check has to run wherever
    a release is cut, and "the toolchain happened to be on PATH" is not a
    property worth depending on for something this important.
    """
    d = open(path, "rb").read()
    pe = struct.unpack("<I", d[0x3c:0x40])[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("%s is not a PE file" % path)
    nsec, = struct.unpack("<H", d[pe + 6:pe + 8])
    optsz, = struct.unpack("<H", d[pe + 20:pe + 22])
    opt = pe + 24
    plus = struct.unpack("<H", d[opt:opt + 2])[0] == 0x20b
    osver = struct.unpack("<HH", d[opt + 40:opt + 44])
    ddir = opt + (112 if plus else 96)
    imp_rva = struct.unpack("<I", d[ddir + 8:ddir + 12])[0]

    secs = []
    sh = pe + 24 + optsz
    for i in range(nsec):
        s = d[sh + i * 40:sh + (i + 1) * 40]
        vsize, vaddr, rawsize, rawptr = struct.unpack("<IIII", s[8:24])
        secs.append((vaddr, max(vsize, rawsize), rawptr))

    def off(rva):
        for vaddr, size, rawptr in secs:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        return None

    libs, p = [], off(imp_rva)
    while p is not None:
        desc = d[p:p + 20]
        if len(desc) < 20 or desc == b"\0" * 20:
            break
        name_rva = struct.unpack("<I", desc[12:16])[0]
        if not name_rva:
            break
        o = off(name_rva)
        libs.append(d[o:d.index(b"\0", o)].decode("ascii").lower())
        p += 20
    return {"bits": 64 if plus else 32, "os": osver, "libs": sorted(set(libs))}


def check_binary(path, want_bits):
    """-> [complaints]. Empty means it will load where we say it will."""
    bad = []
    try:
        info = pe_info(path)
    except Exception as e:
        return ["%s: cannot read the headers (%s)" % (path, e)]
    if info["bits"] != want_bits:
        bad.append("%s is %d-bit, expected %d"
                   % (path, info["bits"], want_bits))
    if info["os"] > MIN_OS:
        bad.append("%s declares minimum OS %d.%02d; Windows %d.%02d would "
                   "refuse to load it"
                   % (path, info["os"][0], info["os"][1], MIN_OS[0], MIN_OS[1]))
    for lib in info["libs"]:
        if lib not in ALLOWED_IMPORTS:
            bad.append("%s imports %s, which is not guaranteed to exist on "
                       "the oldest Windows we claim to support" % (path, lib))
    return bad


def manifest_version():
    path = os.path.join(ADDON, "manifest.ini")
    for line in open(path, encoding="utf-8"):
        if line.strip().startswith("version"):
            return line.split("=", 1)[1].strip()
    raise SystemExit("no version in manifest.ini")


def main():
    version = manifest_version()
    out = os.path.join(ROOT, "outspoken-%s.nvda-addon" % version)

    files, refused = [], []
    for dirpath, dirs, names in os.walk(ADDON):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for n in names:
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, ADDON).replace("\\", "/")
            why = forbidden(rel, n)
            if why:
                refused.append(why)
            else:
                files.append((full, rel))

    # Both binaries, taken from build/ rather than from whatever happens to be
    # sitting in the add-on folder after a deploy.
    missing, unsound = [], []
    for name, rel, bits in DLLS:
        src = os.path.join(BUILD, name)
        if os.path.isfile(src):
            unsound += check_binary(src, bits)
            files = [(f, r) for f, r in files if r != rel]
            files.append((src, rel))
        else:
            missing.append(name)

    if refused:
        print("REFUSING to package. These are not ours to distribute:")
        for r in sorted(set(refused)):
            print("   " + r)
        print("\nThe engine is supplied by the user, from their own copy.\n"
              "See README.md and tools/extract_rom.py.")
        return 1
    if missing:
        print("missing %s -- run `sh build.sh`, which produces both"
              % " and ".join(missing))
        return 1
    if unsound:
        print("REFUSING to package. These binaries would not run everywhere "
              "the manifest says they will:")
        for u in unsound:
            print("   " + u)
        return 1

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for full, rel in sorted(files, key=lambda x: x[1]):
            z.write(full, rel)

    print("wrote %s" % os.path.relpath(out, ROOT))
    print("  %d files, %d KB" % (len(files), os.path.getsize(out) // 1024))
    for name, rel, bits in DLLS:
        info = pe_info(os.path.join(BUILD, name))
        print("  %-44s %d-bit, min OS %d.%02d, needs %s"
              % (rel, info["bits"], info["os"][0], info["os"][1],
                 " ".join(info["libs"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
