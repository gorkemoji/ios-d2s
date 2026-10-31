#!/bin/sh
# Build the D2SGPU test for iOS 16 arm64 (needs Xcode; runs on the macOS CI
# runner, see .github/workflows/d2s-gpu-tools.yml), ad-hoc signed:
#   out/d2sgpu-test        command-line program
#   out/d2sgpu-test.dylib  the same test, run once inside backboardd
#                          (inserted with the d2senv boot-arg)
set -eu
cd "$(dirname "$0")"
mkdir -p out
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Werror"
LIBS="-framework IOKit -framework IOSurface -framework CoreFoundation"
$CC -o out/d2sgpu-test d2sgpu-test.c $LIBS
$CC -dynamiclib -DD2SGPU_DYLIB -install_name /usr/lib/d2sgpu-test.dylib \
	-o out/d2sgpu-test.dylib d2sgpu-test.c $LIBS
for f in out/d2sgpu-test out/d2sgpu-test.dylib; do
	codesign -f -s - "$f"
	file "$f"
done
