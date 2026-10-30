#!/usr/bin/env python3
"""One-step builder for Galaxy Note 10+ (SM-N975F / d2s / Exynos 9825) iOS XNU Boot Image.

Automates:
1. Rebuilding the EL1 shim loader (shim/d2s/out/Image)
2. Generating the Apple Device Tree (d2s.adt)
3. Packing kernel Mach-O + ADT + boot-args into a SWEETXNU payload
4. Assembling loader + payload + Exynos DTB into a unified kernel blob
5. Wrapping into a Samsung SEANDROIDENFORCE Android boot image
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_CMDLINE = (
    "debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 "
    "ufsroot=iosroot unentitled_ios_sim_launch=1 ignition_level=1 "
    "wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 "
    "wdt=-1 a5fb_noconsole=1 sweetfb_hz=15 a5keys=1 s8touch=1 "
    "sweetbattery=0 cs_enforcement_disable=1 amfi_get_out_of_my_way=1"
)

def run(cmd, cwd=None):
    print(f"[>] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str))
    if res.returncode != 0:
        print(f"[!] Command failed with return code {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, default=REPO_ROOT / "stock_boot.img",
                        help="Path to stock Samsung Android boot.img (supplies 2KB header)")
    parser.add_argument("--kernel", type=Path,
                        default=REPO_ROOT / "xnu_out" / "kernel.development.d2s_touch_v2",
                        help="Path to XNU Mach-O kernel")
    parser.add_argument("--dtb", type=Path, default=REPO_ROOT / "tools" / "d2s_merged.dtb",
                        help="Path to d2s_merged.dtb")
    parser.add_argument("--canvas", default="1125x2436",
                        help="Display canvas presented to iOS (default: 1125x2436 for iPhone X layout)")
    parser.add_argument("--cmdline", default=DEFAULT_CMDLINE,
                        help="Kernel command-line arguments")
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "boot_d2s_xnu.img",
                        help="Output boot image path")
    parser.add_argument("--adt", type=Path,
                        help="Use this ADT instead of regenerating d2s.adt "
                             "(e.g. one from tools/d2s_adt_add_cpus.py)")
    parser.add_argument("--no-build-shim", action="store_true",
                        help="Skip recompiling shim/d2s/out/Image")
    parser.add_argument("--files", type=Path,
                        help="tools/mkfiles.py blob the kernel installs into the iOS root at boot")
    args = parser.parse_args()

    # 1. Compile shim
    shim_dir = REPO_ROOT / "shim" / "d2s"
    shim_img = shim_dir / "out" / "Image"
    if not args.no_build_shim:
        print("[*] Step 1: Compiling d2s EL1 shim loader...")
        run(["./build.sh"], cwd=shim_dir)
    if not shim_img.exists():
        sys.exit(f"Error: {shim_img} does not exist. Run build.sh inside shim/d2s.")

    # 2. Build ADT
    if args.adt:
        adt_path = args.adt
        print(f"[*] Step 2: Using Apple Device Tree {adt_path}")
    else:
        adt_path = REPO_ROOT / "d2s.adt"
        print(f"[*] Step 2: Generating Apple Device Tree ({args.canvas})...")
        run([sys.executable, str(REPO_ROOT / "tools" / "mkadt_d2s.py"), str(adt_path), "--canvas", args.canvas])

    # 3. Pack Payload
    payload_path = REPO_ROOT / "payload_d2s.bin"
    print("[*] Step 3: Packing SWEETXNU payload...")
    run([
        sys.executable,
        str(REPO_ROOT / "tools" / "mkpayload.py"),
        str(args.kernel),
        str(adt_path),
        str(payload_path),
        "--cmdline",
        args.cmdline
    ] + (["--files", str(args.files)] if args.files else []))

    # 4. Merge Loader + Payload + DTB
    print("[*] Step 4: Assembling kernel blob (loader @ 0x0, payload @ 0x40000, dtb appended)...")
    loader_bytes = shim_img.read_bytes()
    loader_padded = loader_bytes.ljust(0x40000, b"\x00")
    payload_bytes = payload_path.read_bytes()
    dtb_bytes = args.dtb.read_bytes()
    kblob = loader_padded + payload_bytes + dtb_bytes
    kblob_path = REPO_ROOT / "kblob_d2s.bin"
    kblob_path.write_bytes(kblob)
    print(f"    Kernel blob size: {len(kblob)} bytes")

    # 5. Pack Samsung Boot Image
    if not args.stock.exists():
        print(f"[!] Warning: Stock boot image {args.stock} not found!")
        print("    Please provide a stock boot.img via --stock <path> to generate the final flashable image.")
        print(f"    Intermediate kernel blob saved to: {kblob_path}")
        return

    print("[*] Step 5: Packaging into Samsung Android boot image...")
    run([
        sys.executable,
        str(REPO_ROOT / "tools" / "d2s_mkboot.py"),
        str(args.stock),
        str(kblob_path),
        "/dev/null",
        str(args.out)
    ])
    print(f"\n[+] SUCCESS: Created {args.out} ({args.out.stat().st_size} bytes)")
    print(f"    Flash with: fastboot flash boot {args.out.name}")
    print(f"    Or via ADB (in Recovery):")
    print(f"      adb push {args.out.name} /tmp/boot.img")
    print("      adb shell 'dd if=/tmp/boot.img of=/dev/block/platform/13d60000.ufs/by-name/boot bs=4096 && sync'")
    print("      adb reboot")

if __name__ == "__main__":
    main()
