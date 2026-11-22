#!/bin/bash
# Stage the lapis bq28z610 gauge reader (device/xnu-lapis/driver/
# sweet_lapis_battery.c); patch 0088 adds it to bsd/conf/files and makes
# SweetBattery use it.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
root=${LAPIS_PROJECT_ROOT:-}
[[ -n "$root" && -f "$root/device/xnu-lapis/driver/sweet_lapis_battery.c" ]] || {
    echo 'LAPIS_PROJECT_ROOT does not point at the lapis workspace' >&2; exit 1;
}
cp "$root/device/xnu-lapis/driver/sweet_lapis_battery.c" bsd/dev/sweet_lapis_battery.c
echo "Staged lapis battery gauge"
