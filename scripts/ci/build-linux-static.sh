#!/bin/sh
# Builds the fully static Linux release binary inside an Alpine (musl) container, runs the tests and
# packages the result. Used by CI and the release workflow; runs on x86_64 and arm64 alike.
#
#   docker run --rm -v "$PWD":/src -w /src alpine:3.20 sh scripts/ci/build-linux-static.sh <arch> [version]
set -eu

arch="${1:?usage: build-linux-static.sh <x86_64|arm64> [version-suffix]}"
suffix="${2:-}"

apk add --no-cache build-base cmake ninja linux-headers file >/dev/null

build=/tmp/kcf-build
cmake -S . -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DKCF_WARNINGS_AS_ERRORS=ON \
    -DKCF_STATIC_LINK=ON \
    -DKCF_VERSION_SUFFIX="$suffix"
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure

strip "$build/keyboard-chatter-filter"
file "$build/keyboard-chatter-filter"
"$build/keyboard-chatter-filter" version

mkdir -p dist
staging="$(mktemp -d)"
cp "$build/keyboard-chatter-filter" LICENSE README.md "$staging/"
tar -C "$staging" -czf "dist/keyboard-chatter-filter-linux-${arch}.tar.gz" keyboard-chatter-filter LICENSE README.md
rm -rf "$staging"
ls -l dist
