# iOS 16 (Darwin XNU 22.6.0) on Samsung Galaxy Note 10+ (`SM-N975F` / `d2s` / Exynos 9825)

This guide documents the full architecture, current progress, build system, and open challenges for running native Apple iOS 16 userspace on the **Samsung Galaxy Note 10+** dual-booting with Android.

---

## 1. Project Overview & Hardware Architecture

- **Target Device:** Samsung Galaxy Note 10+ (Global Exynos, Model: `SM-N975F`, Codename: `d2s`)
- **SoC:** Samsung Exynos 9825 (7nm EUV)
  - **CPUs (8 cores):**
    - 4x Cortex-A55 @ 1.95 GHz (LITTLE, MPIDR `0x000` - `0x003`)
    - 2x Cortex-A75 @ 2.40 GHz (MID, MPIDR `0x004` - `0x005`)
    - 2x Mongoose M4 @ 2.73 GHz (BIG, MPIDR `0x100` - `0x101`)
  - **Interrupt Controller:** ARM GIC-400 (**GICv2**, GICD: `0x10101000`, GICC: `0x10102000`)
  - **Storage:** Internal UFS 3.0 (`13d60000.ufs` / `sda`)
  - **Display:** 6.8" Dynamic AMOLED (1440x3040 @ 60Hz, Framebuffer: `0xca000000`)
  - **Touchscreen Controller:** Samsung **S6SY771** (`sec,sec_ts`) on `hsi2c_10` (`0x104b0000`)
- **Software Stack:**
  - **Kernel:** Apple XNU Darwin 22.6.0 (`xnu-8796.141.3`) compiled for ARM64 with Exynos platform patches
  - **Userspace:** iOS 16.7.16 root filesystem (`launchd` -> `backboardd` -> `SpringBoard` GUI)
  - **Root Partition:** UFS partition `sda33` (labeled `iosroot`, ~50 GB), preserving Android `userdata` (`sda32`) untouched for clean dual-boot.

---

## 2. Current Working Status

| Component | Status | Details |
| :--- | :--- | :--- |
| **Boot Chain** | **WORKING** | S-Boot hands off to custom EL1 loader (`shim/d2s`), MMU transitions cleanly, and XNU enters `start_first_cpu`. |
| **Display (AMOLED)** | **WORKING** | Framebuffer at `0xca000000` refreshes cleanly. Console and GUI render on physical display. |
| **iOS 16 GUI / Lock Screen** | **WORKING** | Native iOS 16 Lock Screen renders with Apple red typography ("Wednesday, December 31 - 4:04"), charging icon, camera shortcut, and home bar. |
| **Hardware Buttons (HID)** | **WORKING** | Physical Volume Up & Down trigger the on-screen Apple Volume HUD via `SweetHIDButtons`. Power button toggles display state. |
| **Storage (UFS 3.0)** | **WORKING** | UFS controller (`0x13d60000`) recognized, mounts `sda33` (`iosroot`) read/write via custom SCSI/UFS driver. |
| **Touchscreen** | **IN PROGRESS** | S6SY771 controller on `hsi2c_10` (`0x104b0000`). Pre-boot probe is configured; event reporting loop needs refinement. |
| **Multi-Core (SMP)** | **IN PROGRESS** | Currently boots reliably on single core (`cpu0`). Multi-core bring-up via PSCI with GICv2 is under active development. |

---

## 3. Repository Structure

```text
.
├── D2S_GUIDE.md               # This documentation
├── d2s.adt                    # Compiled Apple Device Tree for Note 10+
├── shim/
│   └── d2s/                   # EL1 pre-boot shim loader for Exynos 9825
│       ├── build.sh           # AArch64 bare-metal loader build script
│       ├── loader.c           # Main loader: relocates XNU segments, setups boot_args
│       ├── start.S            # Assembly entry point (S-Boot handover, stack init)
│       ├── touch_probe.c      # S6SY771 I2C / USI hardware probe & diagnostics
│       └── loader.ld          # Linker script (entry at 0x10008000)
├── tools/
│   ├── d2s_make_boot.py       # ALL-IN-ONE build script to produce boot.img
│   ├── d2s_mkboot.py          # Samsung Android boot image v1 generator
│   ├── mkadt_d2s.py           # Apple Device Tree (ADT) generator for d2s
│   ├── mkpayload.py           # SWEETXNU payload generator (Mach-O + ADT + boot-args)
│   ├── adt.py                 # Apple Device Tree python parser / serializer
│   ├── d2s_merged.dtb         # Note 10+ stock Device Tree Blob (reference)
│   └── stage_d2s_iosroot.py   # Script to flash iOS 16 rootfs to sda33
├── xnu/
│   └── patches/               # 124+ patches for Apple XNU (Samsung Exynos platform)
└── xnu_out/
    └── kernel.development.d2s_touch_v2   # Pre-compiled working XNU Mach-O kernel
```

---

## 4. Prerequisites & Setup

### A. Host System (macOS or Linux)
1. **AArch64 Toolchain:**
   - **macOS:** `brew install aarch64-elf-binutils aarch64-elf-gcc`
   - **Ubuntu/Debian:** `sudo apt-get install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu`
2. **Python 3.8+**
3. **Android Tools:** `adb` and `fastboot`

### B. Device Setup (Galaxy Note 10+)
1. Unlocked bootloader with **OrangeFox** or **TWRP Recovery** installed.
2. Backup copy of your stock Android `boot.img` (saved as `stock_boot.img` in the repo root).
3. Dual-boot partition `sda33` created on internal UFS (50 GB).

---

## 5. Build Commands

### Quick One-Step Build
To compile the shim loader, build the device tree, pack the payload, and output a flashable Samsung boot image in one command:

```bash
python3 tools/d2s_make_boot.py \
    --stock /path/to/stock_boot.img \
    --kernel xnu_out/kernel.development.d2s_touch_v2 \
    --out boot_d2s_xnu.img
```

### Manual Step-by-Step Build

If you are modifying individual components:

#### 1. Compile the EL1 Shim Loader
```bash
cd shim/d2s
./build.sh
cd ../..
# Outputs: shim/d2s/out/Image
```

#### 2. Generate the Apple Device Tree (ADT)
SpringBoard on iOS 16 requires real iPhone canvas dimensions (`1125x2436` @ 3x for iPhone X geometry) to present the lock screen surface without layout assertions:
```bash
python3 tools/mkadt_d2s.py d2s.adt --canvas 1125x2436
```

#### 3. Pack the SWEETXNU Payload
Combines the compiled XNU Mach-O kernel, the ADT, and boot command-line arguments:
```bash
python3 tools/mkpayload.py \
    xnu_out/kernel.development.d2s_touch_v2 \
    d2s.adt \
    payload_d2s.bin \
    --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=iosroot unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=15 a5keys=1 s8touch=1 sweetbattery=0 cs_enforcement_disable=1 amfi_get_out_of_my_way=1"
```

#### 4. Merge Kernel Blob
Pad the loader to 256 KiB (`0x40000`), append the payload, and append the Exynos DTB:
```bash
python3 -c '
loader = open("shim/d2s/out/Image", "rb").read().ljust(0x40000, b"\x00")
payload = open("payload_d2s.bin", "rb").read()
dtb = open("tools/d2s_merged.dtb", "rb").read()
open("kblob_d2s.bin", "wb").write(loader + payload + dtb)
'
```

#### 5. Package into Samsung Android Boot Image
```bash
python3 tools/d2s_mkboot.py /path/to/stock_boot.img kblob_d2s.bin /dev/null boot_d2s_xnu.img
```

---

## 6. Flashing to Device

### Method 1: Via ADB (Device in OrangeFox / TWRP)
```bash
adb push boot_d2s_xnu.img /tmp/boot_d2s_xnu.img
adb shell "dd if=/tmp/boot_d2s_xnu.img of=/dev/block/platform/13d60000.ufs/by-name/boot bs=4096 && sync"
adb reboot
```

### Method 2: Via Fastboot / Download Mode
```bash
fastboot flash boot boot_d2s_xnu.img
fastboot reboot
```

---

## 7. Key Architecture & Hardware Offsets (Exynos 9825)

| Subsystem | Physical Address (PA) | Description |
| :--- | :--- | :--- |
| **DRAM High Bank** | `0x900000000` | Clean high DRAM bank (above firmware carve-outs) handed to XNU. |
| **Framebuffer** | `0xca000000` | Exynos DECON display controller base (`1440x3040`, 32-bit ARGB). |
| **UFS 3.0 Controller** | `0x13d60000` | Universal Flash Storage controller (UFS HCI registers). |
| **GIC-400 (GICv2)** | `0x10101000` (GICD) / `0x10102000` (GICC) | Core interrupt controller. |
| **Touchscreen (HSI2C 10)** | `0x104b0000` | Samsung High-Speed I2C controller for S6SY771 touch IC. |
| **Touch USI Block** | `0x104b00c0` | Universal Serial Interface control (`USI_CONFIG` @ `+0xC0`, `USI_CON` @ `+0xC4`). |
| **Touch SCL / SDA Pins** | `0x10430020` | `gpp1-4` (SCL) and `gpp1-5` (SDA) on `pinctrl@10430000` (Function 2). |
| **Touch IRQ Pin** | `0x15850020` | `gpa1-0` on `pinctrl@15850000` (Bank 1, DAT at offset `+0x24`). |
| **Ramoops Console** | `0xeb308000` | Persistent RAM console buffer (read via `/sys/fs/pstore/console-ramoops-0`). |

---

## 8. Open Collaboration Tasks

### Task A: Touchscreen Driver (`S6SY771`)
- **Location:** `shim/d2s/touch_probe.c` and `xnu/patches/0124-dreamlte-s8.patch` (`s8ts_thread`).
- **Goal:** Get the I2C event loop reading coordinates from `0x104b0000` and forwarding them via `sweet_hid_touch(tid, down, x, y)` into `SweetHIDTouch` (`IOHIDDigitizerEvent`).

### Task B: SMP Multi-Core Enablement
- **Goal:** Secondary CPUs (Cortex-A75 / Mongoose M4) brought online via standard ARM PSCI `CPU_ON` calls (`0xc4000003`), configuring GICv2 CPU interface without triggering GICv3 system register faults.

---

## 9. Reading Debug Logs
If the system crashes or reboots into recovery, the full early boot log from the loader and XNU is preserved in pstore memory:
```bash
adb shell "cat /sys/fs/pstore/console-ramoops-0"
```
Or for kernel messages:
```bash
adb shell "cat /sys/fs/pstore/pmsg-ramoops-0"
```
