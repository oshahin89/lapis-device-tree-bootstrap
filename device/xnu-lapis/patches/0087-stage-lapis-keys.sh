#!/bin/bash
# Stage the lapis buttons driver (device/xnu-lapis/driver/sweet_lapis_keys.c)
# into the ephemeral XNU build tree; patch 0087 adds it to bsd/conf/files.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
root=${LAPIS_PROJECT_ROOT:-}
[[ -n "$root" && -f "$root/device/xnu-lapis/driver/sweet_lapis_keys.c" ]] || {
    echo 'LAPIS_PROJECT_ROOT does not point at the lapis workspace' >&2; exit 1;
}
cp "$root/device/xnu-lapis/driver/sweet_lapis_keys.c" bsd/dev/sweet_lapis_keys.c
echo "Staged lapis keys driver"
