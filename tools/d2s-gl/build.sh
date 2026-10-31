#!/bin/bash
# Build Mesa's panfrost Gallium driver for iOS 16 arm64 and pantest.dylib
# (GPU step 4a). Runs on a macOS runner with Xcode and Homebrew
# (.github/workflows/d2s-gl.yml); output in tools/d2s-gl/out.
#
#  1. Mesa mesa-26.2.6, with tools/d2s-gl/patch-mesa.py applied
#  2. native build of the host tools panfrost needs to compile its internal
#     OpenCL C shaders (mesa_clc, vtn_bindgen2, panfrost_compile; LLVM,
#     SPIRV-LLVM-Translator and libclc from Homebrew)
#  3. iOS cross build of the static libraries with those tools
#     (-Dmesa-clc=system -Dprecomp-compiler=system: no LLVM for iOS), libdrm
#     replaced by tools/d2s-gl/libdrm + d2sdrm.c
#  4. pantest.dylib: the static libraries + d2sdrm.c + pantest.c, compiled
#     with the flags Mesa used for pan_screen.c
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
W=${WORK:-$HERE/work}
OUT=$HERE/out
MESA_TAG=mesa-26.2.6
mkdir -p "$W" "$OUT"

[ -d "$W/mesa" ] || git clone -q --depth 1 --branch $MESA_TAG https://gitlab.freedesktop.org/mesa/mesa.git "$W/mesa"
python3 "$HERE/patch-mesa.py" "$W/mesa"

mkdir -p "$W/pc"
sed "s#@PREFIX@#$HERE/libdrm#" "$HERE/libdrm/libdrm.pc.in" > "$W/pc/libdrm.pc"

BREW=$(brew --prefix)
export PATH="$BREW/opt/llvm/bin:$BREW/opt/bison/bin:$BREW/opt/flex/bin:$PATH"
COMMON=(-Dbuildtype=release -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms=
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true
	-Dshader-cache=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled
	-Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled)

echo "=== native host tools"
if [ ! -f "$W/native/build.ninja" ]; then
	PKG_CONFIG_PATH="$W/pc:$BREW/opt/llvm/lib/pkgconfig:$BREW/lib/pkgconfig:$BREW/share/pkgconfig" \
	meson setup "$W/native" "$W/mesa" "${COMMON[@]}" -Dllvm=enabled \
		-Dmesa-clc=enabled -Dprecomp-compiler=enabled
fi
ninja -C "$W/native" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
	src/panfrost/clc/panfrost_compile
mkdir -p "$W/bin"
cp "$W/native/src/compiler/clc/mesa_clc" "$W/native/src/compiler/spirv/vtn_bindgen2" \
	"$W/native/src/panfrost/clc/panfrost_compile" "$W/bin/"
export PATH="$W/bin:$PATH"

echo "=== iOS cross build"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
AR=$(xcrun --sdk iphoneos -f ar)
FLAGS="'-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0'"
cat > "$W/ios-cross.txt" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
objc = '$CC'
ar = '$AR'
strip = 'strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = [$FLAGS]
cpp_args = [$FLAGS]
objc_args = [$FLAGS]
c_link_args = [$FLAGS]
cpp_link_args = [$FLAGS]

[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$W/pc']

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF
if [ ! -f "$W/ios/build.ninja" ]; then
	meson setup "$W/ios" "$W/mesa" --cross-file "$W/ios-cross.txt" "${COMMON[@]}" \
		-Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
		-Ddefault_library=static -Db_ndebug=true
fi
ninja -C "$W/ios" -k 0

echo "=== pantest.dylib"
python3 - "$W/ios" > "$W/pan_flags.txt" <<'PY'
import json, shlex, sys
build = sys.argv[1]
for e in json.load(open(build + '/compile_commands.json')):
    if e['file'].endswith('src/gallium/drivers/panfrost/pan_screen.c'):
        args = shlex.split(e['command'])[1:]
        keep, skip = [], False
        for a in args:
            if skip:
                skip = False
                continue
            if a in ('-o', '-MF', '-MQ', '-MT'):
                skip = True
                continue
            if a in ('-c', '-MD') or a.endswith('pan_screen.c'):
                continue
            # make relative include paths absolute (they are relative to the build dir)
            if a.startswith('-I') and not a.startswith('-I/'):
                a = '-I' + build + '/' + a[2:]
            keep.append(a)
        print(' '.join(shlex.quote(a) for a in keep))
        break
PY
eval "PANFLAGS=($(cat "$W/pan_flags.txt"))"
[ ${#PANFLAGS[@]} -gt 0 ] || { echo "no compile flags for pan_screen.c"; exit 1; }
INC=(-I"$W/mesa/include" -I"$W/mesa/src" -I"$W/mesa/src/gallium/include"
	-I"$W/mesa/src/gallium/auxiliary" -I"$W/mesa/src/gallium/auxiliary/pipe-loader"
	-I"$W/mesa/src/gallium/drivers" -I"$HERE/libdrm" -I"$REPO/tools/d2s-gpu" -I"$HERE")
cd "$W/ios"
"$CC" -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -Wall \
	"${INC[@]}" -c "$HERE/d2sdrm.c" -o "$W/d2sdrm.o"
"$CC" "${PANFLAGS[@]}" "${INC[@]}" -Wno-error -c "$HERE/pantest.c" -o "$W/pantest.o"
LIBS=$(find "$W/ios/src" -name '*.a' | sort)
echo "$LIBS" | sed "s#$W/ios/##" > "$OUT/libs.txt"
set +e
"$CXX" -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -dynamiclib \
	-install_name /usr/lib/pantest.dylib -Wl,-dead_strip \
	-o "$OUT/pantest.dylib" "$W/pantest.o" "$W/d2sdrm.o" $LIBS \
	-framework IOKit -framework IOSurface -framework CoreFoundation -lz > "$OUT/link.log" 2>&1
rc=$?
set -e
grep -v 'warning:' "$OUT/link.log" | head -80
[ $rc -eq 0 ] || { echo "link failed ($rc)"; exit 1; }
codesign -f -s - "$OUT/pantest.dylib"
file "$OUT/pantest.dylib"
ls -la "$OUT/pantest.dylib"
