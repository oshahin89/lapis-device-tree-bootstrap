#!/bin/bash
# Stage the lapis GPU driver (device/xnu-lapis/driver/sweet_lapis_gpu.c) into
# the ephemeral XNU build tree; patch 0091 adds it to bsd/conf/files and starts
# it from sweet_ufs.c. The Mali firmware is not part of the build: the dual
# boot image carries it (tools/lapis_boot_pack.py --gpufw).
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
root=${LAPIS_PROJECT_ROOT:-}
[[ -n "$root" && -f "$root/device/xnu-lapis/driver/sweet_lapis_gpu.c" ]] || {
    echo 'LAPIS_PROJECT_ROOT does not point at the lapis workspace' >&2; exit 1;
}
cp "$root/device/xnu-lapis/driver/sweet_lapis_gpu.c" bsd/dev/sweet_lapis_gpu.c
echo "Staged lapis GPU driver"
