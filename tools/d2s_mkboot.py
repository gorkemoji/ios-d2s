#!/usr/bin/env python3
"""Build a Samsung Note 10+ (Exynos 9825, d2s) boot.img v1/v2.

Uses stock header parameters (kaddr 0x10008000, raddr 0x11000000, page 2048).
Appends SEANDROIDENFORCE for S-Boot compatibility.
Usage: python3 tools/d2s_mkboot.py stock_boot.img shim_payload ramdisk out.img
"""
import hashlib
import struct
import sys

if len(sys.argv) < 5:
    print("Usage: d2s_mkboot.py <stock_boot.img> <kernel/shim> <ramdisk> <out.img>")
    sys.exit(1)

stock = open(sys.argv[1], 'rb').read()
kernel = open(sys.argv[2], 'rb').read()
ramdisk = open(sys.argv[3], 'rb').read()

assert stock[:8] == b'ANDROID!', 'Not an Android boot image'
(ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, header_version, osver) = struct.unpack_from('<10I', stock, 8)

print(f"[+] Stock parameters: kaddr={kaddr:#x}, raddr={raddr:#x}, page={page}, header_version={header_version}")

def pages(n):
    return (n + page - 1) // page * page

# SHA1 digest (kernel, ramdisk, second)
sha = hashlib.sha1()
for blob in (kernel, ramdisk, b''):
    sha.update(blob)
    sha.update(struct.pack('<I', len(blob)))
digest = sha.digest() + b'\0' * 12

hdr = bytearray(stock[:page])
# ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, header_version, osver
struct.pack_into('<10I', hdr, 8, len(kernel), kaddr, len(ramdisk), raddr, 0, saddr, tags, page, header_version, osver)

# cmdline temizliği (S-Boot kendi cmdline'ını DTB'den enjekte eder)
hdr[0x40:0x240] = b'\0' * 0x200
hdr[0x240:0x260] = digest

out = bytearray(hdr)
out.extend(kernel)
out.extend(b'\0' * (pages(len(kernel)) - len(kernel)))

out.extend(ramdisk)
out.extend(b'\0' * (pages(len(ramdisk)) - len(ramdisk)))

# Samsung SEANDROIDENFORCE footer
out.extend(b'SEANDROIDENFORCE\0\0\0\0')

with open(sys.argv[4], 'wb') as f:
    f.write(out)

print(f"[+] Başarıyla üretildi: {sys.argv[4]} ({len(out)} bytes)")
