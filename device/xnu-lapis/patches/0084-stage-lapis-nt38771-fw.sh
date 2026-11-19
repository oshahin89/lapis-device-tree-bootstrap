#!/bin/bash
# Embed the device's TianMa NT38771 firmware in the ephemeral XNU build tree.
# The firmware is already present in the checked-out workspace; do not fetch
# or redistribute another copy.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
fw=${LAPIS_NT38771_FIRMWARE:-}
[[ -n "$fw" && -f "$fw" ]] || { echo 'LAPIS_NT38771_FIRMWARE is missing' >&2; exit 1; }
root=${LAPIS_PROJECT_ROOT:-}
[[ -n "$root" && -f "$root/device/nt38771_report_parser.c" ]] || {
    echo 'LAPIS_PROJECT_ROOT does not point at the lapis workspace' >&2; exit 1;
}
expected=7722bb9c4f0df6abba2297309ac09036e499956dcc0ee66133a976a2def180fd
actual=$(shasum -a 256 "$fw" | awk '{print $1}')
[[ "$actual" == "$expected" ]] || { echo "unexpected NT38771 firmware SHA-256: $actual" >&2; exit 1; }
python3 - "$fw" <<'PY'
from pathlib import Path
import sys

src = Path(sys.argv[1]).read_bytes()
dst = Path('bsd/dev/lapis_nt38771_fw_data.h')
if dst.exists():
    raise SystemExit(f'refusing to overwrite {dst}')
if len(src) != 196608 or src[0x2f000:0x2f002] != bytes((0x29, 0xd6)):
    raise SystemExit('NT38771 firmware size/version check failed')
with dst.open('w') as out:
    out.write('/* Generated from the checked-in lapis TianMa firmware. */\n')
    out.write('static const unsigned char lapis_nt38771_fw[] = {\n')
    for off in range(0, len(src), 16):
        out.write('\t' + ', '.join(f'0x{x:02x}' for x in src[off:off+16]) + ',\n')
    out.write('};\n')
PY
for src in nt38771_report_parser.c nt38771_report_parser.h \
           nt38771_touch_state.c nt38771_touch_state.h \
           nt38771_pipeline.c nt38771_pipeline.h; do
    cp "$root/device/$src" "bsd/dev/$src"
done
cp "$root/device/xnu-lapis/driver/sweet_nt38771.c" bsd/dev/sweet_nt38771.c
echo "Staged verified lapis NT38771 firmware ($actual)"
