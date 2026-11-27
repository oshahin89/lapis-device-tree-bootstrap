#!/bin/bash
# Stage the lapis haptic (AW86927), backlight (DSI DCS 0x51) and audio
# (AFE + TFA9865) drivers into the ephemeral XNU build tree; patch 0089 adds
# them to bsd/conf/files, starts them from sweet_ufs.c and adds SweetBacklight.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
root=${LAPIS_PROJECT_ROOT:-}
for f in sweet_lapis_haptic.c sweet_lapis_display.c sweet_lapis_audio.c lapis_audio_queue.h lapis_mic_hw_probe.h; do
    [[ -n "$root" && -f "$root/device/xnu-lapis/driver/$f" ]] || {
        echo 'LAPIS_PROJECT_ROOT does not point at the lapis workspace' >&2; exit 1;
    }
    cp "$root/device/xnu-lapis/driver/$f" "bsd/dev/$f"
done
echo "Staged lapis haptic, backlight and audio drivers"
