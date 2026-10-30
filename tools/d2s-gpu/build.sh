#!/bin/sh
# Build d2sgpu-test for iOS 16 arm64 (needs Xcode; runs on the macOS CI
# runner, see .github/workflows/d2s-gpu-tools.yml). Output: out/d2sgpu-test,
# ad-hoc signed.
set -eu
cd "$(dirname "$0")"
mkdir -p out
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 \
	-O2 -Wall -Wextra -Werror -o out/d2sgpu-test d2sgpu-test.c \
	-framework IOKit -framework CoreFoundation
codesign -f -s - out/d2sgpu-test
file out/d2sgpu-test
