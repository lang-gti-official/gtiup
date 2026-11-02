#!/bin/sh
set -eu
# Runs inside the pinned Alpine CI container, not on an end user's machine.
apk add --no-cache build-base cmake ninja python3 openssl-dev openssl-libs-static zlib-dev zlib-static ca-certificates linux-headers git
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DGTIUP_STATIC=ON
cmake --build build-linux --target gtiup gtiup-test --parallel 4
ctest --test-dir build-linux --output-on-failure
# A static executable must not depend on the container's loader or shared libraries.
if readelf -l build-linux/gtiup | grep -q INTERP; then echo 'gtiup must be fully static' >&2; exit 1; fi
python3 scripts/package.py --build build-linux --platform linux-x64
