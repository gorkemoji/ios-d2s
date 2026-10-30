#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
IMG="$SCRIPT_DIR/../../ios16-root.sparseimage"

if [[ ! -f "$IMG" ]]; then
    echo "Hata: $IMG bulunamadi!"
    exit 1
fi

echo "[*] Telefon ADB baglantisi kontrol ediliyor..."
adb devices

echo "[*] sparseimage disk aygiti olarak baglaniyor..."
DEV=$(hdiutil attach "$IMG" -nomount | grep Apple_HFS | awk '{print $1}')

if [[ -z "$DEV" ]]; then
    echo "Hata: Apple_HFS aygiti baglanamadi!"
    exit 1
fi

echo "[+] Baglanan aygit: $DEV"
RAW_DEV="/dev/r${DEV#/dev/}"

echo "[*] HFS partisyonu Note 10+'a (sda33 / iosroot) yaziliyor..."
echo "    (Bu islem USB hizina bagli olarak ~2-3 dakika surebilir)"

# dd stream over adb
sudo dd if="$RAW_DEV" bs=4m status=progress | adb shell "dd of=/dev/block/sda33 bs=4m"

echo "[*] UFS onbellegi senkronize ediliyor..."
adb shell "sync"

echo "[*] Disk ayriliyor..."
BASE_DISK=$(echo "$DEV" | sed -E 's/s[0-9]+$//')
hdiutil detach "$BASE_DISK" -force

echo ""
echo "=========================================================="
echo "  [+] BASARIYLA TAMAMLANDI: Tam iOS RootFS telefona yazildi!"
echo "=========================================================="

