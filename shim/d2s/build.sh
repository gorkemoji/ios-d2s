#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

if command -v aarch64-elf-gcc >/dev/null 2>&1; then
    PREFIX="aarch64-elf-"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    PREFIX="aarch64-linux-gnu-"
else
    echo "Hata: aarch64 cross compiler bulunamadı!" >&2
    exit 1
fi

CC="${PREFIX}gcc"
OBJCOPY="${PREFIX}objcopy"
READELF="${PREFIX}readelf"
NM="${PREFIX}nm"

CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror"

mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c loader.c -o out/loader.o
$CC $CFLAGS -c ../fdt.c -o out/fdt.o
$CC $CFLAGS -c ufs_probe.c -o out/ufs_probe.o
$CC $CFLAGS -c smp_probe.c -o out/smp_probe.o
$CC $CFLAGS -c touch_probe.c -o out/touch_probe.o
$CC $CFLAGS -c pmic.c -o out/pmic.o
$CC $CFLAGS -c s2mps19.c -o out/s2mps19.o
$CC $CFLAGS -c gpu_probe.c -o out/gpu_probe.o
$CC $CFLAGS -c display_probe.c -o out/display_probe.o
$CC $CFLAGS -c sd_probe.c -o out/sd_probe.o

objs="out/loader.o out/fdt.o out/ufs_probe.o out/smp_probe.o out/touch_probe.o out/s2mps19.o out/gpu_probe.o out/pmic.o out/display_probe.o out/sd_probe.o"

abs=$($READELF -r $objs | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "Hata: $abs absolute relocations tespit edildi" >&2; exit 1; }

$CC $CFLAGS -static -T loader.ld -Wl,--build-id=none -Wl,-N \
	out/start.o $objs -o out/loader.elf

$OBJCOPY -O binary out/loader.elf out/Image

size=$(( 0x$($NM out/loader.elf | awk '$3=="_end"{print $1}') ))
truncate -s "$size" out/Image

if stat -f %z out/Image >/dev/null 2>&1; then
    img_size=$(stat -f %z out/Image)
else
    img_size=$(stat -c %s out/Image)
fi
echo "[+] out/Image derlendi: $img_size bytes"

if [ $# = 3 ]; then
    python3 ../../tools/d2s_mkboot.py "$1" out/Image "$2" "$3"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$3"
    else
        shasum -a 256 "$3"
    fi
fi
