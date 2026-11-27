#!/bin/sh
# Compile and exercise the microphone support components with synthetic data.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/lapis-mic-host.XXXXXX")
CC=${CC:-cc}
for name in pmif mic_queue mic_cursor mic_pcm mic_pipeline; do
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
        "$ROOT/tools/lapis_audio_probe/${name}_check.c" -o "$OUT/$name"
    "$OUT/$name"
done
printf '\001\000\000\200\000\000' > "$OUT/fixture"
"$CC" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "-DLAPIS_MIC_SELFTEST_DEVICE=\"$OUT/fixture\"" -DLAPIS_MIC_SELFTEST_DELAY_SEC=0 \
    -DLAPIS_MIC_SELFTEST_DURATION_NS=20000000ULL \
    "$ROOT/tools/lapis_audio_probe/mic_live_check.c" -o "$OUT/live"
"$OUT/live" 2> "$OUT/live.log"
grep -F 'frames=3 nonzero=2 peak=32768 errors=0 odd=0' "$OUT/live.log" >/dev/null
printf 'Host-only microphone support checks passed. No hardware was accessed.\n'
