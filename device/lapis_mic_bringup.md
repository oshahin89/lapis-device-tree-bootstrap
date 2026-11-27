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
