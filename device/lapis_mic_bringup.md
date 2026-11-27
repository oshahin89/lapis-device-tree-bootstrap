# Lapis microphone bring-up

## Android mixer snapshot, 2026-11-27

The user recorded on Android while `tools/lapis_android_mic_snapshot.sh` took
one baseline and eight ten-second snapshots. All 733 mixer controls were read
in each snapshot. Sample 1 matched baseline; samples 2–8 consistently differed
in 16 controls. The diagnostic captured mixer and ALSA metadata, not PCM.
The script found no readable capture `hw_params` paths, so the sample rate,
format and capture endpoint are not established by this capture.

| Control | Baseline | Recording |
| --- | --- | --- |
| `adsp_capture_sharemem_scenario` | Off | On |
| `UL9_CH1 ADDA_UL_CH1`, `UL9_CH2 ADDA_UL_CH2` | Off | On |
| `UL_CM1_CH1 ADDA_UL_CH1`, `UL_CM1_CH2 ADDA_UL_CH2` | Off | On |
| `CM1_UL_MUX` | `CM1_2CH_PATH` | `CM1_16CH_PATH` |
| `PGA1 Volume`, `PGA2 Volume` | 4, 0 | 1, 1 |
| `dsp_captureul1_runtime_en`, `dsp_captureraw_runtime_en` | Off | On |
| `MISO0_MUX`, `MISO1_MUX` | `UL1_CH1`, `UL1_CH2` | `UL1_CH2`, `UL1_CH1` |
| `DMIC0_MUX`, `DMIC1_MUX` | `DMIC_DATA0`, `DMIC_DATA1_L_1` | `DMIC_DATA1_L_1`, `DMIC_DATA0` |
| `ADC_R_Mux` | Idle | Right Preamplifier |
| `PGA_R_Mux` | None | AIN2 |

The settings indicate an AIN2/right ADC path and active DSP capture in this
Android recording session. They do not establish the physical microphone's
location, register values, sample format, whether the DSP is strictly required,
or the sequence needed for an independent XNU implementation.

An independently published [MT6369 codec driver](https://github.com/mt6878-devs/android_kernel_device_modules/blob/034b70b1cbe1bb56f134bcb9d462daaec89aec7b/sound/soc/codecs/mt6369.c)
for the MT6878 family associates the same right ADC and PGA mux names with
registers described in its [MT6369 header](https://github.com/mt6878-devs/android_kernel_device_modules/blob/034b70b1cbe1bb56f134bcb9d462daaec89aec7b/sound/soc/codecs/mt6369.h).
It is a useful cross-check, not proof this device uses identical board wiring
or initialization. The next diagnostic captures Android regmap/clock events
through a private trace instance around record start and stop. Only after
matching the device's own trace and identifying a safe sample path should an
opt-in XNU microphone probe be attempted.

The current iOS HAL code can ask for `/dev/hakios-mic`, but Lapis does not yet
implement that device; missing input is padded with silence. Speaker playback,
CPU rendering and persistent audio should be preserved during microphone work.

## Android register trace and XNU probe preparation

The user's private trace had 15,105 entries in the buffer and 15,105 written.
Around 1213.66–1213.70 seconds, writes from the audio capture thread reached
the `0-05` regmap and the MT6878 AFE; around 1230.79 seconds related settings
were reversed. The saved Android boot log identifies `0-05` as chip ID `0x69`.
The codec writes include `AUDENC_ANA_CON2/3` (`0x250a/0x250b`), microphone
bias (`0x2519`), and the UL digital control (`0x238b`). The trace records writes
but not initial values or all hardware state, so it is not a replay script.

The Android AFE writes identify VUL9 base/end `0x11059000..0x110643ff`, then
VUL9 control at `0x4f30`. This observed region overlaps all 8,064 bytes of the
existing XNU speaker ring at `0x11060000..0x11061f7f`. Android's captured
address range must not be reused concurrently with that ring. Buffer allocation,
cache coherency, the producer/consumer cursor, overrun handling, routing and
format require separate design. `tools/lapis_mic_trace_analyze.py` summarizes
the relevant writes and reports overlap without exposing audio samples.

The traced VUL9 control value `0x100a49` has sample-rate field `0xa` in bits
8–12. The independently published MT6878 driver maps that encoding to 48 kHz
([control mapping](https://github.com/mt6878-devs/android_kernel_device_modules/blob/034b70b1cbe1bb56f134bcb9d462daaec89aec7b/sound/soc/mediatek/mt6878/mt6878-afe-control.c),
[rate constants](https://github.com/mt6878-devs/android_kernel_device_modules/blob/034b70b1cbe1bb56f134bcb9d462daaec89aec7b/sound/soc/mediatek/common/mtk-base-afe.h)).
This is an inference about that hardware capture stage; the app-facing format
and channel packing were not measured.

The same control value has the published `VUL9_ON` bit 28 clear. The change
from `0x100a48` to `0x100a49` changes bit 0 (HD mode), not bit 28 (enable).
Do not interpret that pair as proof that the AP enabled VUL9. The trace records
Linux regmap writes, not necessarily direct MMIO or DSP writes. The observed
`hd_mode=1`, together with the generic FE driver's format mapping, is not
enough on its own to prove the app-facing PCM format or sample packing.

The existing XNU speaker worker initializes shared AFE control registers on
each speaker restart and turns parts of the output path off after inactivity.
An input implementation must coordinate shared clocks and AFE ownership with
that worker; blindly replaying Android's start/stop writes could disrupt audio
output even if a separate capture buffer were selected.

On the first XNU read-only probe boot, the user confirmed working speaker and
volume buttons. The log identified MT6369 software ID `0x69` and successfully
read all ten requested codec registers; `0x250a`, `0x250b`, and `0x2519` were
zero. This establishes register reachability at idle, not ADC operation or
capture. The probe performs its reads on the key polling thread to serialize
the existing PMIF channel; any future independent microphone worker needs a
shared PMIF transaction lock or another verified coordination mechanism.

## Next implementation boundaries

1. Provide serialized PMIF transactions for the keys and microphone paths,
   with bounded timeouts. Do not replay the Android register stream verbatim:
   regmap writes omit reads, prior state, and work done by the DSP.
2. Manage shared AFE power and clocks across input and output. The current
   speaker worker can reset shared controls when it starts; input and output
   cannot safely own those controls independently.
3. Establish an independently owned, DMA-accessible capture buffer with
   known cache behavior and a hardware write cursor; do not use Android's
   overlapping VUL9 range. Bound and detect overruns.
4. Determine channel layout and sample representation before converting to
   the HAL's 48 kHz mono signed 16-bit nonblocking input contract. Missing
   samples must be zero-filled, not stale memory.
5. Test microphone enable/disable, repeated capture, volume buttons and
   simultaneous speaker activity. Initial diagnostics should report state and
   counts without saving or transmitting microphone recordings.

`device/xnu-lapis/driver/lapis_pmif.h` contains a hardware-independent
single-byte PMIF transaction core. Its host check exercises normal reads and
writes, stale-valid handling, invalid inputs and bounded timeout paths under
address and undefined-behavior sanitizers. The keys driver now prepares a
shared mutex and exposes a serialized read adapter; if lock allocation fails,
external access is refused and key polling retains its original fallback.
The opt-in `lapispmifcheck=1` starts a second thread that reads the codec ID
100 times while key polling runs. This is prepared source, not yet validated
on the phone. No shared codec-writing interface is exposed by this step.

A timed-out write has unknown hardware outcome, and an idle controller alone
does not prove codec acceptance: stable fields must be read back when a later
hardware experiment makes that safe. Any future read/modify/write operation
must hold the same mutex for the complete sequence.

MT6363 key polling already uses PMIF SWINF channel 2. A separate microphone
thread using that channel without synchronization could corrupt transactions.
The opt-in `lapismicprobe=1` preparation performs only codec register reads,
once on the existing keys thread. It avoids AFE MMIO until the clock state
is established. It
must first be compiled and validated on-device; it does not enable the ADC,
create `/dev/hakios-mic`, or prove microphone capture.
