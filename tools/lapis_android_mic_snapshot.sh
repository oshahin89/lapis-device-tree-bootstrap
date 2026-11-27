#!/system/bin/sh
# Capture mixer state and ALSA stream parameters only; no audio data is saved.
set -u
PATH=/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/data/local/tmp:$PATH
export PATH
OUT=/sdcard/Download/lapis-mic-state.txt
if [ "$(id -u)" != 0 ]; then echo 'Run with su'; exit 1; fi
TM=$(command -v tinymix 2>/dev/null || true)
if [ -z "$TM" ]; then
 for candidate in /data/local/tmp/tinymix /vendor/bin/tinymix /system/bin/tinymix; do
  if [ -x "$candidate" ]; then TM=$candidate; break; fi
 done
fi
if [ -z "$TM" ]; then echo 'tinymix not found. Send this message to Codex.'; exit 1; fi
snapshot() {
 printf '\n--- %s ---\n' "$1"
 date
 printf '%s\n' '[mixer]'
 "$TM" 2>&1
 printf '%s\n' '[capture PCM parameters]'
 for f in /proc/asound/card*/pcm*c/sub*/hw_params; do
  if [ -r "$f" ]; then printf '\n%s\n' "$f"; cat "$f"; fi
 done
}
if [ -e "$OUT" ]; then echo "Output exists: $OUT; rename it before retrying"; exit 1; fi
printf 'Capturing baseline. Next, open Android Recorder and start recording; keep it running for 75 seconds.\n'
snapshot 'baseline' > "$OUT"
printf 'Baseline saved. Start recording now; return to Termux after about 80 seconds.\n'
for n in 1 2 3 4 5 6 7 8; do
 sleep 10
 snapshot "sample $n" >> "$OUT"
done
printf 'Done. Stop recording and send /sdcard/Download/lapis-mic-state.txt\n'
