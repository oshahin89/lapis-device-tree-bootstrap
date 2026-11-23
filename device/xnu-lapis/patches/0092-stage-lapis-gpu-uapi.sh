#!/bin/bash
# Stage LapisGPU (device/xnu-lapis/driver/LapisGPU.cpp: the GPU for programs,
# panthor's requests over an IOKit user client) into the ephemeral XNU build
# tree; patch 0092 adds it to iokit/conf/files.arm64. sweet_lapis_gpu.c starts
# it once the GPU is up.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
root=${LAPIS_PROJECT_ROOT:-}
[[ -n "$root" && -f "$root/device/xnu-lapis/driver/LapisGPU.cpp" ]] || {
    echo 'LAPIS_PROJECT_ROOT does not point at the lapis workspace' >&2; exit 1;
}
cp "$root/device/xnu-lapis/driver/LapisGPU.cpp" iokit/Kernel/arm/LapisGPU.cpp
echo "Staged LapisGPU"
