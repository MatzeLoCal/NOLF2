#!/usr/bin/env python3
"""
rezlist.py — list or extract entries from a LithTech .rez archive, by PARSING
THE INDEX.

    macbuild/tools/rezlist.py <file.rez> [name-substring ...]
    macbuild/tools/rezlist.py <file.rez> --extract <path-substring> <outdir>

★ WHY THIS EXISTS
Byte-grepping a .rez for filenames has produced a wrong answer at least twice
(PHASE2_HANDOFF, "BEWARE SUBSTRING GREPS — AGAIN": a mod byte-grepped as
83 DTX / 77 WAV actually contained FOUR entries; the strings lived inside its
bundled Object.lto). The only reliable answer comes from walking the directory
tree, which is what this does.

Format, from libs/rezmgr/rezmgr.cpp (structs are #pragma pack(1)):

  FileMainHeaderStruct   at offset 0
      CR LF  FileType[60]  CR LF  UserTitle[60]  CR LF  EOF(0x1A)
      then 10 x DWORD:  FileFormatVersion, RootDirPos, RootDirSize,
                        RootDirTime, NextWritePos, Time, LargestKeyAry,
                        LargestDirNameSize, LargestRezNameSize,
                        LargestCommentSize
      then BYTE IsSorted

  A directory is a run of entries, each:
      DWORD Type              0 = resource, 1 = directory
    Type 1 (directory):  DWORD Pos, Size, Time,  then NUL-terminated Name
    Type 0 (resource):   DWORD Pos, Size, Time, ID, Type(4cc), NumKeys,
                         then NUL-terminated Name, NUL-terminated Comment,
                         then NumKeys x DWORD

⚠️ The resource `Type` is a 4-character code stored little-endian, so it reads
back reversed ("VAW " for a .wav). The on-disk NAME carries no extension — the
extension IS the type code.
"""

import struct
import sys
import os

HDR_FMT = "<2s60s2s60s3s10I B"


def _read_header(b):
    # 2 + 60 + 2 + 60 + 3 = 127 bytes of text, then the DWORDs.
    off = 127
    vals = struct.unpack_from("<10I", b, off)
    return {
        "FileFormatVersion": vals[0],
        "RootDirPos": vals[1],
        "RootDirSize": vals[2],
        "RootDirTime": vals[3],
        "NextWritePos": vals[4],
    }


def _cstr(b, off):
    end = b.index(b"\0", off)
    return b[off:end].decode("latin-1"), end + 1


def _fourcc(n):
    s = struct.pack("<I", n).decode("latin-1").strip("\0 ")
    return s[::-1].strip()          # stored reversed; see module docstring


def walk(b, pos, size, prefix, out):
    """Walk one directory's entry run, recursing into subdirectories."""
    end = pos + size
    off = pos
    while off < end:
        if off + 4 > len(b):
            return
        (etype,) = struct.unpack_from("<I", b, off)
        off += 4
        if etype == 1:                                    # directory
            dpos, dsize, _t = struct.unpack_from("<3I", b, off)
            off += 12
            name, off = _cstr(b, off)
            walk(b, dpos, dsize, prefix + name + "/", out)
        elif etype == 0:                                  # resource
            rpos, rsize, _t, _id, rtype, nkeys = struct.unpack_from("<6I", b, off)
            off += 24
            name, off = _cstr(b, off)
            _comment, off = _cstr(b, off)
            off += 4 * nkeys
            ext = _fourcc(rtype)
            full = prefix + name + ("." + ext if ext else "")
            out.append((full, rpos, rsize))
        else:
            return                                        # desynced; stop


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    h = _read_header(b)
    out = []
    walk(b, h["RootDirPos"], h["RootDirSize"], "", out)
    return b, out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    b, entries = load(path)

    if len(sys.argv) >= 5 and sys.argv[2] == "--extract":
        pat, outdir = sys.argv[3].lower(), sys.argv[4]
        os.makedirs(outdir, exist_ok=True)
        n = 0
        for name, pos, size in entries:
            if pat in name.lower():
                dst = os.path.join(outdir, os.path.basename(name))
                with open(dst, "wb") as f:
                    f.write(b[pos:pos + size])
                print("extracted %s (%d bytes) -> %s" % (name, size, dst))
                n += 1
        print("%d file(s)" % n)
        return 0

    pats = [a.lower() for a in sys.argv[2:]]
    total = 0
    for name, pos, size in entries:
        if not pats or any(p in name.lower() for p in pats):
            print("%10d  %s" % (size, name))
            total += 1
    print("--- %d of %d entries in %s" % (total, len(entries), os.path.basename(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
