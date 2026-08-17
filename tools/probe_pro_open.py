# -*- coding: utf-8 -*-
"""Open MacinTalk Pro as the Component Manager would, and report what it wants.

Milestone 1 for Pro, the same shape `probe_mt2_open.py` has for MacinTalk 2.
The point is the logs, not the return code: an unknown $A82A selector halts
with its stack intact, and every Toolbox trap is listed whether or not we
served it, because that list is the real specification of what the host still
owes this engine.

What is known before running it, measured rather than recalled:

  * `thng 128` decodes as type `ttsc`, **subtype 0** (MacinTalk 2's is 'mtk2'),
    manufacturer `gala`, code in `gtse 1`.  So Pro is ONE component, where
    MacinTalk 2 is a front end and a back end that opens the other.
  * `gtse 1` is 35,202 bytes and opens with the identical standard component
    entry -- `movea.l $c(a6),a3` then `tst.w $2(a3)` on the selector -- so
    every line of Component Manager glue already written serves it.
  * A Pro voice is nothing like a MacinTalk 2 one: `ttvd` + a 128-byte `gtsv`
    record + `gtss` holding both the concatenative unit database (789-922 KB)
    and about 11 KB of per-voice code.

Two things this is expected to run into, and neither should be built for until
the log asks:

  * **The 572,928-byte data fork.**  On a real Macintosh the engine reads its
    own file's data fork for the lexicon, through File Manager traps this host
    has never needed.  If `_Open`/`_Read`/`_SetFPos` appear below, that is the
    day's real work, and the host has no notion of "the extension's own file"
    to hang it on.
  * **Heap arithmetic.**  Resources are copied into emulated RAM, and one
    voice is more than MacinTalk 2's entire heap, so the map below is redrawn
    rather than inherited.

    py -3 tools/probe_pro_open.py
    py -3 tools/probe_pro_open.py Victoria
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
import paths                                                   # noqa: E402
import voices as voicelib                                      # noqa: E402
from disasm import trap_name                                   # noqa: E402
from probe_mt2_open import CM_NAMES, signed                     # noqa: E402

#: Redrawn for Pro.  Victoria's `gtss 3` alone is 921 KB where MacinTalk 2's
#: whole heap is 917 KB, and `osp_add_resource` copies into emulated RAM.  RAM
#: is 16 MB with the host's magic pages from 0x00F00000, so there is room --
#: but MacinTalk 2's TEXT_BUF at 0x195000 would sit inside this heap, which is
#: why none of those constants are reused.
CODE = 0x00040000
HEAP = 0x00080000
HEAP_SIZE = 0x00A00000                 # 10 MB, ends at 0x00A80000
STACK = 0x00C00000
TEXT_BUF = 0x00C10000
VOICE_SPEC = 0x00C20000
STATUS_BUF = 0x00C20100
PARAM_BUF = 0x00C20200

CPUFLAG, RESERR, MEMERR = 0x012F, 0x0A60, 0x0220

OPEN, CLOSE, CANDO, VERSION = -1, -2, -3, -4

#: `thng` is the component descriptor -- the Component Manager's own
#: bookkeeping, never something the code asks the Resource Manager for.
NOT_A_RESOURCE = ("thng",)


def engine_dir():
    for root in paths.roots():
        d = os.path.join(root, "macintalkpro")
        if os.path.isdir(d):
            return d
    raise SystemExit(
        "rom/macintalkpro is empty.\n"
        "Extract MacinTalk Pro from your own disk image:\n"
        "    py -3 tools/extract_rom.py \"C:/path/to/MacOS7.hfv\"")


def split(name):
    """`gtse_1.bin` -> ('gtse', 1).  Returns None for anything else."""
    stem = os.path.splitext(name)[0]
    if "_" not in stem:
        return None
    rtype, rest = stem.rsplit("_", 1)
    try:
        return rtype, int(rest)
    except ValueError:
        return None


def read_names(folder):
    """-> {(type, id): (map entry, mac name)} from `resources.tsv`.

    A Mac resource name is not a file name -- Pro's modules are `*TTS`,
    `*Wave`, `*Lex` -- so the index lives beside the `.bin`s rather than in
    them, and carries the map offset RsrcMapEntry answers with. Absent means
    an extraction from before either was kept.
    """
    out = {}
    p = os.path.join(folder, "resources.tsv")
    if not os.path.isfile(p):
        return out
    with open(p, encoding="utf-8") as fh:
        for line in fh:
            row = line.rstrip(chr(13) + chr(10))
            if row.startswith("#") or "	" not in row:
                continue
            rtype, rid, entry, nm = row.split("	", 3)
            out[(rtype, int(rid))] = (int(entry), nm)
    return out


def build(want=None, quiet=False, snap=None, halt=0):
    """Register everything and Open the component.

    -> (host, instance token, voice, Open result). Shared with
    probe_pro_speak.py, because getting Pro to the point of being open is
    twenty lines of exact detail and having two copies of it would mean
    fixing the next discovery twice.
    """
    def say(*a):
        if not quiet:
            print(*a)

    d = engine_dir()

    code_path = os.path.join(d, "gtse_1.bin")
    if not os.path.isfile(code_path):
        raise SystemExit("no gtse_1.bin in %s -- that is the engine itself" % d)
    code = open(code_path, "rb").read()
    say("MacinTalk Pro  gtse 1  %d bytes at 0x%X" % (len(code), CODE))

    allv, _bad = voicelib.installed("gala")
    if not allv:
        raise SystemExit("no MacinTalk Pro voices under rom/voices")
    voice = None
    for v in allv:
        if want is None or v.name == want:
            voice = v
            break
    if voice is None:
        raise SystemExit("no such Pro voice: %s (have %s)"
                         % (want, ", ".join(v.name for v in allv)))
    say("voice          %s, %s, %d files" % (voice.name, voice.gender,
                                               len(os.listdir(voice.folder))))

    h = osp.Host()
    # MacinTalk Pro's Open reads Gestalt('proc') and refuses 1 or 68000 and 2
    # or 68010 outright -- see gtse 1 +$282, which writes #$ff0f, that is
    # synthOpenFailed. So "Pro" is a hardware requirement, and this must be set
    # before any code is loaded.
    h.set_cpu(osp.Host.CPU_68020)
    h.load(CODE, code)
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)
    h.w16(RESERR, 0)
    h.w16(MEMERR, 0)

    # The forks first, because a resource has to be tagged with the file it
    # came from and the file has to exist to be tagged with. Neither fork is a
    # resource, and they are not interchangeable: the engine's lexicon is in
    # ITS data fork, and a voice's 800 KB of units is in the VOICE file's
    # resource fork, which Pro reads by walking the map and seeking.
    where = {}
    for label, folder, macname in (("engine", d, "MacinTalk Pro"),
                                   ("voice", voice.folder, voice.name)):
        dat = rf = b""
        pd = os.path.join(folder, "datafork.bin")
        pr = os.path.join(folder, "rsrcfork.bin")
        if os.path.isfile(pd):
            dat = open(pd, "rb").read()
        if os.path.isfile(pr):
            rf = open(pr, "rb").read()
        if not dat and not rf:
            say("  ! %s has neither fork -- re-run tools/extract_rom.py" % label)
            continue
        h.add_file(macname, dat, rf)
        where[label] = len(where)
        say("file           %-14s data %7d, resource %7d"
            % (macname, len(dat), len(rf)))

    # Everything the engine ships, then everything the voice ships, each tagged
    # with its file. **The same type and id mean different things in different
    # files**: `gtsg 0` is 1,032 bytes in the engine and 110 in a voice, so a
    # file-blind table hands Pro the wrong one.
    #
    # The host holds 64 resources; Pro is 50 once `thng` is set aside and a
    # voice is 10, so ONE voice fits and three do not. That is a real
    # constraint on the eventual module, not a limitation of this probe.
    n, skipped, ttvd_id, named = 0, [], None, 0
    for label, folder in (("engine", d), ("voice", voice.folder)):
        names = read_names(folder)
        fidx = where.get(label, -1)
        for name in sorted(os.listdir(folder)):
            if name in ("datafork.bin", "rsrcfork.bin", "resources.tsv",
                        "names.tsv"):
                continue
            got = split(name)
            if not got:
                continue
            rtype, rid = got
            if rtype in NOT_A_RESOURCE:
                skipped.append("%s %d" % (rtype, rid))
                continue
            data = open(os.path.join(folder, name), "rb").read()
            try:
                hnd = h.add_resource(rtype, rid, data, fidx)
            except RuntimeError as e:
                say("  ! %s %d (%d bytes): %s" % (rtype, rid, len(data), e))
                continue
            # Pro finds its modules by name -- `*TTS`, `EnglMBruceData` -- and
            # asks RsrcMapEntry where each sits before reading it out of the
            # file, so both ride on the Handle we just got back.
            entry, mac = names.get((rtype, rid), (0, ""))
            if entry:
                h.map_entry(hnd, entry)
            if mac and h.name_resource(hnd, mac):
                named += 1
            if rtype == "ttvd" and label == "voice":
                ttvd_id = rid
            n += 1
    say("registered     %d resources, %d of them named (%s set aside), "
        "heap %d KB of %d KB"
        % (n, named, " ".join(skipped) or "none",
           h.lib.osp_heap_used() // 1024, HEAP_SIZE // 1024))
    if not named:
        say("  ! no names -- re-run tools/extract_rom.py; Pro finds its\n"
            "    modules by name and cannot open without them")

    # Which file the voice lives in, and it matters. Pro asks the Speech
    # Manager for the voice's FSSpec -- GetVoiceInfo('fref') -- and then OPENS
    # it. A nominal spec with an empty name, which is all MacinTalk 2 ever
    # needed, sent it to the engine's own fork looking for the voice's units.
    if ttvd_id is not None:
        h.add_voice(voice.creator, voice.id, ttvd_id, where.get("voice", -1))

    # thng 128: type 'ttsc', subtype 0, manufacturer 'gala'.
    comp = h.add_component("ttsc", b"\0\0\0\0", "gala", CODE)
    tok = h.open_instance(comp)
    say("component      %d, instance 0x%08X\n" % (comp, tok))

    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.SR, 0x2700)
    # `snap` is an offset into gtse 1 to watch during Open itself. Reading
    # emulated memory settles in one run what the disassembly kept getting
    # wrong -- gtse 1 is 68020 code and the listing desynchronises after every
    # long multiply and bitfield instruction.
    if snap is not None:
        h.snap_at(CODE + snap, halt_on=halt)

    reason, result = h.component_call(tok, OPEN, [tok], max_instr=200_000_000)
    return h, tok, voice, (reason, result)


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else None
    h, tok, voice, (reason, result) = build(want)

    print("$A82A calls, in order:")
    if not h.cm_log:
        print("  (none -- it never reached its own glue)")
    for d0, pc, csp, words, served in h.cm_log:
        print("  +%05X  D0=0x%08X  %-34s %s"
              % (pc - CODE, d0, CM_NAMES.get(d0, "*** UNKNOWN ***"),
                 "" if served else "<-- HALTED HERE"))
        if not served:
            print("           stack at 0x%08X: %s"
                  % (csp, " ".join("0x%08X" % w for w in words)))

    print("\nToolbox and OS traps:")
    if not h.traps:
        print("  (none)")
    seen = {}
    for i, (pc, word, d0, a0, a1, served) in enumerate(h.traps):
        nm = trap_name(word)
        seen.setdefault(nm, [0, served])[0] += 1
        d0in = h.trap_d0in(i)
        chars = "".join(chr(c) if 32 <= c < 127 else "."
                        for c in d0in.to_bytes(4, "big"))
        print("  +%05X  %-24s D0in=0x%08X %-7s A0=0x%08X %s"
              % (pc - CODE if pc >= CODE else pc, nm, d0in,
                 "'%s'" % chars if d0in > 0x20202020 else "", a0,
                 "" if served else "<-- STUBBED"))

    unserved = sorted(k for k, (_c, s) in seen.items() if not s)
    if unserved:
        print("\n  NOT SERVED, and this is the list that matters:")
        for k in unserved:
            print("     %-24s x%d" % (k, seen[k][0]))

    print("\nresources it asked for:")
    for rtype, rid, found in h.resource_requests:
        print("  %s %-6d %s" % (rtype, rid, "" if found else "<-- NOT FOUND"))

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
    print("  heap used:     %d KB" % (h.lib.osp_heap_used() // 1024))
    print("  MemErr:        %d" % signed(h.r16(MEMERR)))
    print("  faults:        %d" % h.fault_count)
    for addr, pc, wr, sz in h.faults[:8]:
        print("      %s%d at 0x%08X from 0x%X"
              % ("write" if wr else "read", sz * 8, addr, pc))

    stor = h.instance_storage(tok)
    print("\n  storage handle 0x%X" % stor)
    if stor:
        blk = h.r32(stor)
        print("  storage block  0x%X" % blk)
    return 0 if reason == 1 else 1


if __name__ == "__main__":
    sys.exit(main())
