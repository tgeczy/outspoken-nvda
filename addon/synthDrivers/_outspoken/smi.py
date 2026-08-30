# -*- coding: utf-8 -*-
"""Read a self-mounting `.smi` disk image down to the speech files inside it.

Someone downloads a classic Mac speech package off an archive site as MacBinary
`.bin` files and has no emulator.  `ospextract` already reads a raw `.hfv` and a
bare MacBinary resource fork; a **self-mounting image** is the wrapper those
downloads actually arrive in, and this module is the four layers between the
`.bin` and the engine.  All four are pure Python -- no emulator, nothing shipped
-- and each is documented in full in `docs/self-mounting-images.md`.

The worked example, and the reason this exists, is the Mexican Spanish MacinTalk
Pro package: **`Mexican_TTS_1.5_{1,2,3}of3.smi.bin`**, three floppies Apple
shipped together.  From them this carves the `cami` engine and the Carlos and
Catalina voices, each reproduced byte-for-byte.

    Layer 1  MacBinary        a 128-byte header; the image is the data fork
    Layer 2  NDIF (bcem/ADC)  an HFS volume stored as raw/compressed/zero chunks
    Layer 3  the tome         Apple Installer 4.0.3 archive; a file catalog
    Layer 4  InstaCompOne     LZ77+Huffman, a sequence of records (see insta3)

Nothing here, and nothing this produces for a release, contains a byte of any
engine.  This is the recipe; the bits stay on the user's own disks.
"""
import struct

import rsrc
import insta3
import _machfs as machfs


# ---------------------------------------------------------------- layer 1 --
#
# MacBinary: a 128-byte header, then the data fork padded to a 128-byte
# boundary, then the resource fork.  A self-mounting image unwraps to an
# `APPL`/`oneb` (a One-Button-Mounter application): the NDIF disk image is its
# data fork, the mounting stub -- including the `bcem` block map -- its
# resource fork.

def _macbin(data):
    """-> (data fork, resource fork) of a MacBinary file, or (None, None)."""
    if len(data) < 128:
        return None, None
    dlen = struct.unpack_from(">I", data, 83)[0]
    rlen = struct.unpack_from(">I", data, 87)[0]
    doff = 128
    roff = 128 + ((dlen + 127) // 128) * 128
    if roff + rlen > len(data):
        return None, None
    return data[doff:doff + dlen], data[roff:roff + rlen]


def identify(data):
    """-> a dict describing a self-mounting image, or None.

    Recognises the wrapper by structure, never by file name: an `APPL`/`oneb`
    whose resource fork carries a `bcem` block map.  The dict names the volume
    (`bcem`'s own Pascal string, e.g. "Mexican TTS 1") so a caller can tell one
    floppy of a set from another without trusting the download's file name.
    """
    if len(data) < 128:
        return None
    ftype = data[65:69]
    creator = data[69:73]
    if ftype != b"APPL" or creator != b"oneb":
        return None
    _df, rf = _macbin(data)
    if rf is None:
        return None
    try:
        bcem = next(r for r in rsrc.parse(rf) if r.type == "bcem")
    except (StopIteration, Exception):
        return None
    d = bcem.data
    nlen = d[4] if len(d) > 4 else 0
    name = d[5:5 + nlen].decode("mac-roman", "replace") if nlen else ""
    return {"volume": name,
            "macbin_name": data[2:2 + data[1]].decode("mac-roman", "replace")}


# ---------------------------------------------------------------- layer 2 --
#
# NDIF: the data fork is an HFS volume stored as a list of chunks, each placed
# at a destination sector.  The `bcem` resource is the map.  ADC (Apple Data
# Compression) is a byte-oriented LZ used for the compressed chunks.

def _adc(src):
    """Apple Data Compression, three opcodes; -> the decompressed bytes."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        b = src[i]
        i += 1
        if b & 0x80:                                   # literal run
            c = (b & 0x7F) + 1
            out += src[i:i + c]
            i += c
        elif b & 0x40:                                 # long match
            c = (b & 0x3F) + 4
            dist = struct.unpack_from(">H", src, i)[0] + 1
            i += 2
            for _ in range(c):
                out.append(out[-dist])
        else:                                          # short match
            c = ((b >> 2) & 0x0F) + 3
            dist = ((b & 0x03) << 8 | src[i]) + 1
            i += 1
            for _ in range(c):
                out.append(out[-dist])
    return bytes(out)


def _ndif_volume(data, res):
    """Reconstruct the HFS volume bytes from the data fork and its `bcem` map.

    Walk the 12-byte chunk entries in order, placing each at its destination
    sector: raw is copied, ADC is decompressed, zero-fill is left as the zeros
    the buffer already holds.
    """
    bcem = next(r for r in rsrc.parse(res) if r.type == "bcem")
    d = bcem.data
    total_sectors = struct.unpack_from(">I", d, 0x44)[0]
    num_chunks = struct.unpack_from(">I", d, 0x7C)[0]
    vol = bytearray(total_sectors * 512)
    for i in range(num_chunks):
        w0, src_off, comp_len = struct.unpack_from(">III", d, 128 + i * 12)
        typ = w0 & 0xFF
        dest = (w0 >> 8) * 512
        if typ == 0xFF:                                # end of map
            break
        if typ == 0x00:                                # zero-fill
            continue
        chunk = data[src_off:src_off + comp_len]
        if typ == 0x02:                                # raw
            vol[dest:dest + len(chunk)] = chunk
        elif typ == 0x83:                              # ADC
            dec = _adc(chunk)
            vol[dest:dest + len(dec)] = dec
        else:
            raise ValueError("unknown NDIF chunk type 0x%02x" % typ)
    return bytes(vol)


def mount(data):
    """A self-mounting `.smi.bin`'s bytes -> a mounted `machfs.Volume`."""
    df, rf = _macbin(data)
    if df is None:
        raise ValueError("not a MacBinary file")
    vol = machfs.Volume()
    vol.read(_ndif_volume(df, rf))
    return vol


# ---------------------------------------------------------------- layer 3 --
#
# The tome: an Apple Installer 4.0.3 archive, one 128-byte section per file,
# each fork a triple (size, offset, compressed size) into the tome's own data
# fork.  Confirmed byte-for-byte against kainjow/TomeViewerX's `tome.c`.

_TOME_MAGIC = 0x6B630001


def tome_sections(tome):
    """-> [section dicts] for one tome's data fork.

    A section names a file (type, creator) and, for each fork, where its
    InstaCompOne stream sits: `rsize` bytes, decompressed, at `roff`.
    """
    if struct.unpack_from(">I", tome, 0)[0] != _TOME_MAGIC:
        raise ValueError("not a tome (bad magic)")
    count = struct.unpack_from(">I", tome, 28)[0]
    secs = []
    for i in range(count):
        base = 36 + i * 128
        nlen = tome[base + 6]
        name = tome[base + 7:base + 7 + nlen].decode("mac-roman", "replace")
        ftype, creator = struct.unpack_from(">4s4s", tome, base + 38)
        rsz, roff, _rcsz = struct.unpack_from(">III", tome, base + 76)
        secs.append({"name": name,
                     "type": ftype.decode("mac-roman", "replace"),
                     "creator": creator.decode("mac-roman", "replace"),
                     "rsize": rsz, "roff": roff})
    return secs


# ---------------------------------------------------------------- layer 4 --
#
# InstaCompOne, as a SEQUENTIAL RECORD STREAM.  A fork is a run of records,
# each a 4-byte header, each contributing output up to the next 64 KiB
# boundary, with the output position and the LZ history GLOBAL across records.
# The codec itself is `insta3`; this is the record framing around it.
#
# It is not a bitstream to scan for `00 01 00 00` markers: an accidental
# `00 01 00 00` occurs inside raw audio, and a marker scan stops there and
# leaves the rest of the fork as garbage.  Consuming every record consumes
# every source byte.

_QUANTUM = 65536
_COMPRESSED = b"\x00\x01\x00\x00"
_RAW = b"\x01\x01\x00\x00"


def _decode_into(buf, off, out, upto, tick=None):
    """Decode records from `buf` at `off` into `out` (global history) until
    `len(out)` reaches `upto`.  Calls `tick(n)` with each quantum's size."""
    pos = off
    while len(out) < upto:
        hdr = bytes(buf[pos:pos + 4])
        pos += 4
        boundary = min(((len(out) // _QUANTUM) + 1) * _QUANTUM, upto)
        if hdr == _COMPRESSED:
            c = insta3.Ctx(buf, pos)
            mode = 1
            while len(out) < boundary:
                cc0 = insta3.decode_length(c)
                if cc0 > 0 or mode == 0:
                    cc = cc0 + 2 + (1 if mode == 0 else 0)
                    dpos = len(out)
                    mag = dpos if dpos < 32768 else 32768
                    dist = insta3.decode_distance(c, mag)
                    ref = dpos - dist
                    for k in range(cc):
                        s = ref + k
                        out.append(out[s] if s >= 0 else 0)
                    mode = 1
                else:
                    ll = insta3.decode_literal(c)
                    for _ in range(ll):
                        out.append(c.getbits(8))
                    mode = 0 if ll < 63 else 1
            pos = c.pos
        elif hdr == _RAW:
            n = boundary - len(out)
            out.extend(buf[pos:pos + n])
            pos += n
        else:
            raise ValueError("bad InstaCompOne record header at %d: %s"
                             % (pos - 4, hdr.hex()))
        if tick is not None:
            tick(boundary)
    return pos


def _decode(buf, off, usize, tick=None):
    """Decode one single-piece fork -> its bytes."""
    out = bytearray()
    _decode_into(buf, off, out, usize, tick)
    return bytes(out[:usize])


def _is_head(fork):
    """-> the total assembled size if `fork` begins with a valid resource-fork
    header, else None.

    A resource fork opens with dataOffset(4), mapOffset(4), dataLen(4),
    mapLen(4), and dataOffset is always 256.  The head piece of a file split
    across tomes carries the header for the WHOLE fork, so its mapOffset points
    past its own bytes -- which is exactly how the head is told from a tail (a
    raw continuation whose first bytes are arbitrary data).  The total the
    header implies is `mapOffset + mapLen`; the pieces must add up to it.
    """
    if len(fork) < 16:
        return None
    do, mo, dl, ml = struct.unpack_from(">IIII", fork, 0)
    if do == 256 and do + dl == mo and ml > 0:
        return mo + ml
    return None


# --------------------------------------------------------------- assembly --

def carve(floppies, say=None, progress=None):
    """Carve every `cami` speech file out of a set of self-mounting floppies.

    `floppies` is a list of the raw `.smi.bin` bytes, in disk order.  Returns
    `[(name, type, creator, resource-fork bytes)]` -- the engine (`thng`) and
    the voices (`ttvf`), each classified by the tome's own type/creator so a
    MacRoman name like "Espa\xf1ol" never has to be matched.

    A file too large for one 800 KB floppy is split across the set (Catalina:
    a head on disk 2, a tail on disk 1); the pieces are gathered by name,
    ordered head-first, joined, and checked against the size the head's own
    header declares.  `progress(done, total)` reports decoded bytes.
    """
    def out(msg):
        if say is not None:
            say(msg)

    # Layers 1-3: mount each floppy, take its tome's data fork.
    tomes = []                        # [(disk index, tome bytes)]
    for i, blob in enumerate(floppies):
        vol = mount(blob)
        for parts, obj in vol.iter_paths():
            if isinstance(obj, machfs.Folder):
                continue
            if obj.data[:4] == b"" or len(obj.data) < 36:
                continue
            try:
                if struct.unpack_from(">I", obj.data, 0)[0] == _TOME_MAGIC:
                    tomes.append((i, obj.data))
            except struct.error:
                continue
    if not tomes:
        raise ValueError("no tome archive found on any floppy")

    # Layer 3: gather each cami file's pieces across the tomes, in disk order.
    pieces = {}                       # name -> [(disk, tome, section)]
    order = []                        # first-seen name order, for a stable list
    for disk, tome in tomes:
        for sec in tome_sections(tome):
            if sec["creator"] != "cami":
                continue
            if sec["name"] not in pieces:
                pieces[sec["name"]] = []
                order.append(sec["name"])
            pieces[sec["name"]].append((disk, tome, sec))

    total = sum(s["rsize"] for plist in pieces.values() for (_d, _t, s) in plist)
    done = [0]
    results = []
    for name in order:
        plist = pieces[name]
        ftype = plist[0][2]["type"]

        def make_tick(base):
            def _t(boundary):
                done[0] = base + boundary
                if progress is not None:
                    progress(min(done[0], total), total)
            return _t

        base = sum(len(r[3]) for r in results)   # bytes finished before this file
        if len(plist) == 1:
            disk, tome, sec = plist[0]
            fork = _decode(tome, sec["roff"], sec["rsize"], make_tick(base))
        else:
            # Decode each piece, then order head-first.
            decoded = []
            run = base
            for disk, tome, sec in plist:
                d = _decode(tome, sec["roff"], sec["rsize"], make_tick(run))
                decoded.append(d)
                run += sec["rsize"]
            heads = [(d, _is_head(d)) for d in decoded]
            head = [d for d, tot in heads if tot is not None]
            tails = [d for d, tot in heads if tot is None]
            if len(head) != 1:
                raise ValueError(
                    "%s is split across %d tomes but %d begin with a resource "
                    "header; cannot order the pieces"
                    % (name, len(plist), len(head)))
            total_size = _is_head(head[0])
            fork = head[0] + b"".join(tails)
            if len(fork) != total_size:
                raise ValueError(
                    "%s assembled to %d bytes; its header declares %d"
                    % (name, len(fork), total_size))
        # A last self-check: it has to parse as a resource fork.
        try:
            rsrc.parse(fork)
        except Exception as e:
            raise ValueError("%s did not decode to a resource fork: %s"
                             % (name, e))
        out("  carved %-28s %s/cami  %d bytes" % (name[:28], ftype, len(fork)))
        results.append((name, ftype, "cami", fork))
    return results
