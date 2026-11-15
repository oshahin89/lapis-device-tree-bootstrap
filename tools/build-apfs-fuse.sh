#!/bin/bash
# Build apfs-fuse from source (not packaged on Ubuntu 26.04), for mkiosroot.sh.
# libfsapfs cannot read the sealed iOS volumes; apfs-fuse can.
#   tools/build-apfs-fuse.sh [DEST]     (default ~/apfs-fuse-build)
# Needs: cmake g++ make git libfuse3-dev libbz2-dev libattr1-dev zlib1g-dev
# Two fixes for a modern toolchain: cmake_minimum_required 3.0 -> 3.5, and a
# missing <cstdint> include in ApfsLib/PList.h. Prints the apfs-fuse path.
set -euo pipefail
DEST=${1:-$HOME/apfs-fuse-build}
[ -d "$DEST/.git" ] || git clone --recursive https://github.com/sgan81/apfs-fuse.git "$DEST"
grep -rl "cmake_minimum_required(VERSION 3.0)" "$DEST" --include=CMakeLists.txt 2>/dev/null \
  | xargs -r sed -i 's/cmake_minimum_required(VERSION 3\.0)/cmake_minimum_required(VERSION 3.5)/'
grep -q "#include <cstdint>" "$DEST/ApfsLib/PList.h" || \
  sed -i '0,/#include/s//#include <cstdint>\n#include/' "$DEST/ApfsLib/PList.h"
mkdir -p "$DEST/build-out"
( cd "$DEST/build-out" && cmake "$DEST" >/dev/null && make -j"$(nproc)" >/dev/null )
echo "apfs-fuse built: $DEST/build-out/apfs-fuse"
