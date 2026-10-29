#!/bin/bash
# Stage iOS 16.7.16 (20H392) rootfs for Galaxy Note 10+ (d2s / Exynos 9825)
# Must run with sudo to preserve iOS UID/GID contracts (root: 0, _logd: 272, mobile: 501)
set -euo pipefail

if [[ ${EUID} != 0 ]]; then
    echo "Hata: Bu script iOS dosya sahipliklerini (root, mobile, _logd) korumak icin 'sudo' ile calistirilmalidir:"
    echo "  sudo $0"
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/../../.." && pwd)
IPSW_DIR="$REPO_DIR/other_folders/ipsw_extracted"
OUT_DIR="$REPO_DIR/other_folders"
SPARSE_IMG="$OUT_DIR/ios16-root.sparseimage"

OS_DMG="$IPSW_DIR/087-86698-044.dmg"
SYSOS_DMG="$IPSW_DIR/087-86593-047.dmg"
APPOS_DMG="$IPSW_DIR/087-86715-047.dmg"

for dmg in "$OS_DMG" "$SYSOS_DMG" "$APPOS_DMG"; do
    if [[ ! -f "$dmg" ]]; then
        echo "Eksik DMG: $dmg"
        exit 1
    fi
done

# Temizlik kancasi
MOUNTED=()
cleanup() {
    echo "Temizleniyor..."
    for dev in "${MOUNTED[@]}"; do
        hdiutil detach "$dev" -force 2>/dev/null || true
    done
}
trap cleanup EXIT

echo "=== 1. DMG imajlari baglaniyor ==="
MNT_OS=$(hdiutil attach -readonly -nobrowse -noverify "$OS_DMG" | grep Apple_APFS | awk '{print $NF}')
MOUNTED+=("$MNT_OS")
echo "  OS baglandi: $MNT_OS"

MNT_SYS=$(hdiutil attach -readonly -nobrowse -noverify "$SYSOS_DMG" | grep Apple_APFS | awk '{print $NF}')
MOUNTED+=("$MNT_SYS")
echo "  SystemOS baglandi: $MNT_SYS"

MNT_APP=$(hdiutil attach -readonly -nobrowse -noverify "$APPOS_DMG" | grep Apple_APFS | awk '{print $NF}')
MOUNTED+=("$MNT_APP")
echo "  AppOS baglandi: $MNT_APP"

echo "=== 2. HFSX Sparse Image olusturuluyor ==="
rm -f "$SPARSE_IMG"
hdiutil create -size 12g -type SPARSE -fs "Case-sensitive Journaled HFS+" -volname iOSRoot "$OUT_DIR/ios16-root"
MNT_ROOT=$(hdiutil attach "$SPARSE_IMG" -mountpoint /Volumes/iOSRoot -owners on | grep Apple_HFS | awk '{print $NF}')
MOUNTED+=("$MNT_ROOT")
echo "  Hedef baglandi: $MNT_ROOT (owners acik)"

echo "=== 3. OS dosyalari kopyalaniyor (SpringBoard ve kütüphaneler dahil, gereksiz stok appler haric) ==="
omit=(
    '--exclude=/Applications/***'
    '--exclude=/System/Library/LinguisticData/***'
    '--exclude=/System/Library/PreinstalledAssetsV2/***'
    '--exclude=/System/Library/Health/***'
    '--exclude=/System/Library/NanoTimeKit/***'
    '--exclude=/System/Library/Assistant/***'
    '--exclude=/System/Library/OnBoardingBundles/***'
    '--exclude=/System/Library/Audio/***'
)
rsync -aH --numeric-ids --extended-attributes "${omit[@]}" "$MNT_OS/" "$MNT_ROOT/"

echo "=== 4. SystemOS ve AppOS Cryptex'leri kopyalaniyor ==="
mkdir -p "$MNT_ROOT/private/preboot/Cryptexes/OS" "$MNT_ROOT/private/preboot/Cryptexes/App"
rsync -aH --numeric-ids --extended-attributes "$MNT_SYS/" "$MNT_ROOT/private/preboot/Cryptexes/OS/"
rsync -aH --numeric-ids --extended-attributes "$MNT_APP/" "$MNT_ROOT/private/preboot/Cryptexes/App/"

echo "=== 5. dyld_shared_cache ve Cryptex linkleri olusturuluyor ==="
mkdir -p "$MNT_ROOT/System/Library/Caches/com.apple.dyld"
CACHE_SRC="$MNT_ROOT/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
for file in "$CACHE_SRC"/*; do
    [[ -f "$file" ]] || continue
    ln "$file" "$MNT_ROOT/System/Library/Caches/com.apple.dyld/${file##*/}"
done

for cryptex in OS App; do
    src="$MNT_ROOT/private/preboot/Cryptexes/$cryptex"
    while IFS= read -r -d '' file; do
        relative=${file#"$src"/}
        [[ $relative == System/Library/Caches/com.apple.dyld/* ]] && continue
        target="$MNT_ROOT/$relative"
        [[ -e $target || -L $target ]] && continue
        mkdir -p "${target%/*}"
        if [[ -L $file ]]; then
            ln -s "$(readlink "$file")" "$target"
        else
            ln "$file" "$target"
        fi
    done < <(find "$src" ! -type d -print0)
done

echo "=== 6. logd ve AppleKeyStore veritabanlari hazirlaniyor ==="
DB="$MNT_ROOT/private/var/db"
install -d -o 272 -g 272 -m 0751 "$DB/diagnostics"
install -d -o 272 -g 272 -m 0755 "$DB/uuidtext" "$DB/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
    install -o 272 -g 272 -m 0644 /dev/null "$DB/$d/.migrated"
done

install -d -o 0 -g 0 -m 0700 "$MNT_ROOT/private/var/keybags"
cat << 'EOF' > "$MNT_ROOT/private/var/keybags/systembag.kb"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>
EOF
chown 0:0 "$MNT_ROOT/private/var/keybags/systembag.kb"
chmod 0600 "$MNT_ROOT/private/var/keybags/systembag.kb"

echo "=== 7. sweet-root-tweaks uygulaniyor (Setup bypass, SpringBoard watchdogs off) ==="
python3 "$SCRIPT_DIR/sweet-root-tweaks.py" "$MNT_ROOT"

echo "=== 8. Dogrulama ==="
test -f "$MNT_ROOT/sbin/launchd"
test -f "$MNT_ROOT/System/Library/CoreServices/SpringBoard.app/SpringBoard"
test -f "$MNT_ROOT/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
echo "Root boyutu:"
df -h "$MNT_ROOT"

sync
echo "=== Basariyla tamamlandi! ==="

