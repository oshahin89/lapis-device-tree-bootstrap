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
printf 'Host-only microphone support checks passed. No hardware was accessed.\n'
