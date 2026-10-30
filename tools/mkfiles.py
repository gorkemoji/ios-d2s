#!/usr/bin/env python3
"""Pack files for the d2s kernel to install into the iOS root at boot.

  mkfiles.py OUT.bin SPEC [SPEC ...]

SPEC is DEST=SRC[,mode=0755][,uid=0][,gid=0][,if-absent]. DEST is an absolute
path in the iOS root and may contain {hash8}: the first 8 hex digits of the
file's SHA-256, so a new build lands in a new file and never overwrites a
library a running process has loaded. Prints each final DEST.

The kernel side is bsd/kern/d2s_files.c (xnu patch 0133); pass OUT.bin to
tools/d2s_make_boot.py --files. Format: "D2SFILE1", u32 version 1, u32 count,
then 8-byte aligned records:
  u32 size, u16 op (1 write, 2 write if absent), u16 0, u32 mode, u32 uid,
  u32 gid, u32 path_len (with NUL), u32 data_len, u32 0, path, pad, data, pad
"""
import hashlib, struct, sys


def record(dest, data, mode, uid, gid, op):
    path = dest.encode() + b'\0'
    path_pad = path + b'\0' * (-len(path) & 7)
    rec = struct.pack('<IHHIIIIII', 0, op, 0, mode, uid, gid, len(path), len(data), 0)
    rec += path_pad + data
    rec += b'\0' * (-len(rec) & 7)
    return struct.pack('<I', len(rec)) + rec[4:]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, recs = sys.argv[1], []
    for spec in sys.argv[2:]:
        dest_src, *opts = spec.split(',')
        dest, src = dest_src.split('=', 1)
        mode, uid, gid, op = 0o644, 0, 0, 1
        for o in opts:
            if o == 'if-absent':
                op = 2
            else:
                k, v = o.split('=')
                if k == 'mode':
                    mode = int(v, 8)
                elif k == 'uid':
                    uid = int(v)
                elif k == 'gid':
                    gid = int(v)
                else:
                    sys.exit(f'unknown option {o}')
        data = open(src, 'rb').read()
        dest = dest.replace('{hash8}', hashlib.sha256(data).hexdigest()[:8])
        if not dest.startswith('/'):
            sys.exit(f'{dest}: not an absolute path')
        recs.append(record(dest, data, mode, uid, gid, op))
        print(dest)
    blob = b'D2SFILE1' + struct.pack('<II', 1, len(recs)) + b''.join(recs)
    open(out, 'wb').write(blob)


if __name__ == '__main__':
    main()
