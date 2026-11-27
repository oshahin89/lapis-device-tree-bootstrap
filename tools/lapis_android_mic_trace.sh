#!/system/bin/sh
# Read regmap and clock trace events during Android recording; no PCM is read.
set -u
PATH=/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/data/local/tmp:$PATH
export PATH
OUT=/sdcard/Download/lapis-mic-regtrace.txt.gz
[ "$(id -u)" = 0 ] || { echo 'Run with su'; exit 1; }
[ ! -e "$OUT" ] || { echo "Output exists: $OUT; rename it before retrying"; exit 1; }
command -v gzip >/dev/null || { echo 'gzip not found'; exit 1; }
BASE=
for d in /sys/kernel/tracing /sys/kernel/debug/tracing; do
 if [ -d "$d/instances" ]; then BASE=$d; break; fi
done
[ -n "$BASE" ] || { echo 'tracefs instances unavailable'; exit 1; }
INST="$BASE/instances/lapis_mic_$$"
mkdir "$INST" || { echo 'Cannot create private trace instance'; exit 1; }
started=0
cleanup() {
 if [ -d "$INST" ]; then
  echo 0 > "$INST/tracing_on" 2>/dev/null || true
  if [ "$started" = 1 ]; then
   { printf '# Android microphone capture trace; no audio samples\n'; cat "$INST/trace"; } | gzip -c > "$OUT"
  fi
  echo 0 > "$INST/events/enable" 2>/dev/null || true
  rmdir "$INST" 2>/dev/null || true
 fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM
[ -e "$INST/events/regmap/regmap_reg_write/enable" ] || { echo 'regmap trace event unavailable'; exit 1; }
echo 0 > "$INST/tracing_on" || exit 1
echo 2048 > "$INST/buffer_size_kb" || { echo 'Cannot set private trace buffer'; exit 1; }
echo 1 > "$INST/events/regmap/regmap_reg_write/enable" || { echo 'Cannot enable regmap trace'; exit 1; }
for e in clk_enable clk_disable clk_set_rate clk_set_parent; do
 if [ -e "$INST/events/clk/$e/enable" ]; then echo 1 > "$INST/events/clk/$e/enable" 2>/dev/null || true; fi
done
: > "$INST/trace"
echo 1 > "$INST/tracing_on" || exit 1
started=1
printf 'TRACE RUNNING: start Android Recorder now, record for about 15 seconds, then stop. Return after about 45 seconds.\n'
sleep 45
printf 'Capture finished; saving trace to %s\n' "$OUT"
