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

The Lapis runtime device tree gives audio SRAM as `0x11059000..0x11064fff`.
`0x11059000..0x1105cfff` is a 16 KiB capture candidate outside the XNU speaker
ring and inside the range Android used for VUL9. At 48 kHz stereo with
32-bit slots it wraps in about 42.7 ms. Ownership and address acceptance
still require validation before mapping it for capture.

The traced VUL9 control value `0x100a49` has sample-rate field `0xa` in bits
8–12. The independently published MT6878 driver maps that encoding to 48 kHz
([control mapping](https://github.com/mt6878-devs/android_kernel_device_modules/blob/034b70b1cbe1bb56f134bcb9d462daaec89aec7b/sound/soc/mediatek/mt6878/mt6878-afe-control.c),
[rate constants](https://github.com/mt6878-devs/android_kernel_device_modules/blob/034b70b1cbe1bb56f134bcb9d462daaec89aec7b/sound/soc/mediatek/common/mtk-base-afe.h)).
This is an inference about that hardware capture stage; the app-facing format
and channel packing were not measured.

The same `0x100a49` has HD mode 1 and stereo mode selected. In the published
MT6878 FE implementation, with 32-bit memif support enabled and the VUL9
descriptor's default `hd_msb_shift`, this is consistent with `S24_LE` in
32-bit slots. It is not proof of the byte alignment or microphone channel on
this device. `lapis_mic_pcm.h` offers explicit candidate converters for
S16LE stereo and low-justified S24LE-in-32 stereo; host checks cover signed
extremes, channel selection, capacity and incomplete frames. Neither
converter is attached to hardware until the actual sample packing is checked.

The same control value has the published `VUL9_ON` bit 28 clear. The change
from `0x100a48` to `0x100a49` changes bit 0 (HD mode), not bit 28 (enable).
Do not interpret that pair as proof that the AP enabled VUL9. The trace records
Linux regmap writes, not necessarily direct MMIO or DSP writes. The observed
`hd_mode=1`, together with the generic FE driver's format mapping, is not
enough on its own to prove the app-facing PCM format or sample packing.

The trace also shows the SoC UL0 source control at `0xbd0` changing from
`0x60400` to `0x60401` during capture and back on stop; the published MT6878
driver identifies bit 0 as the UL source enable. The recorder also configures
the PMIC audio serial interface before enabling the codec UL source. These
observed transitions are useful evidence, but neither they nor a raw replay
establish the complete clock, pin, DMA or calibration state.

In the captured runtime device tree, the audio clock and data MOSI pins are
164 and 166, and data MISO0/MISO1 are 170 and 171; each `on` pinmux uses
function 1. An Android boot log reports MTKAIF calibration phases 2 and 2 for
that boot. Those values and pin settings do not prove the current iOS boot
retains the same calibration or electrical pin configuration.

The Android trace also records the UL0 ADC clock enable and an MT6363 VS1
vote (`0-04`, `0x189b`) immediately before capture, with matching teardown
(`0x189c`). Confirm both dependencies before enabling the ADC from XNU.
`lapismicsnapshot=1` adds a one-time read of pinmux, MTKAIF, ADDA UL0 and VUL9
configuration after speaker-side AFE clock setup. It can be used in the same
boot as the PMIF probe and does not read sample memory.

`lapismicdmacheck=1` adds a one-shot digital DMA check on that same worker
after DL0 starts. The published MT6878 interconnect routes DL0 to VUL1, so
the check uses that route and a dedicated 8 KiB window at `0x1105b000` to
isolate DMA from codec initialization. It checks that the overlapping engines
seen in the Android trace are idle, checks configuration readback, and reports
cursor bounds and how many SRAM words the engine replaced. It restores the
tested DMA and routing registers after confirming the engine is stopped. It
reports counts only and clears the test buffer. This still needs on-device
validation and does not establish that VUL9 receives microphone input.

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

The published MT6369 DAPM routes explicitly require `ADC_L_EN` when using
`ADC_R`, even for the right-input route. That matches captured writes to both
left and right analog controls. A right-input bring-up must account for this
dependency and check it against the device's clock, bias and speaker state.

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
on the phone. A shared masked update now holds the mutex across read, write,
and readback of stable configuration fields. It is not called by a live
driver yet; a failed or timed-out write has an unknown hardware outcome.

A timed-out write has unknown hardware outcome, and an idle controller alone
does not prove codec acceptance: stable fields must be read back when a later
hardware experiment makes that safe. Any future read/modify/write operation
must hold the same mutex for the complete sequence.

`lapis_mic_queue.h` is a bounded mono S16 queue for the existing HAL contract.
Its host test covers ordering, wraparound, overflow, short reads and erasure.
It is not yet connected to DMA or a device node; hardware sample format,
permissions, lifecycle and concurrent speaker ownership remain to be solved.
The base image's `com.apple.mediaserverd` launchd plist specifies
`UserName = mobile`, so a future input device cannot assume it opens as root.
The effective UID, sandbox behavior and a restrictive access policy still need
verification before exposing actual microphone samples.

`lapis_mic_cursor.h` prepares generic read-side DMA cursor accounting with a
guard region, wrap handling, a maximum per-poll advance and a fail-closed
polling deadline. Its host test covers boundary and error cases. The caller must
verify the real DMA sample rate and format, choose a deadline shorter than a
full ring traversal, copy
offered spans immediately and provide memory ordering; the helper alone cannot
prove that the pointer or samples are valid on this hardware.

`lapis_mic_pipeline.h` combines the cursor guard, candidate PCM conversion,
mono queue and error handling into a hardware-neutral pipeline. It wipes the
queue on a cursor fault and does not register a device or start hardware.
The host-only `mic_pipeline_check.c` feeds it a simulated wrapping DMA ring in
S16LE and S24LE-in-32 and exercises ordering and a late-poll failure. This is
a synthetic software test: no codec, physical DMA, app permissions, hardware
synchronization or real microphone capture has been verified by it.

Run all software-only checks together with
`tools/lapis_audio_probe/run_host_checks.sh`. They build with address and
undefined-behavior sanitizers and do not require a phone or retain recordings.

`mic_live_check.c` prepares an optional app-independent on-device check for a
future `/dev/hakios-mic`: after a 60-second delay it reads for up to two minutes
and reports only frame counts, nonzero counts, peak and errors, wiping each
temporary buffer. It saves no PCM. The optional aggregate-report ioctl is a
proposed contract and has no kernel implementation yet; stderr capture and
on-device timing still need validation. This direct device check cannot prove
AVAudioSession routing or real app recording.

MT6363 key polling already uses PMIF SWINF channel 2. A separate microphone
thread using that channel without synchronization could corrupt transactions.
The opt-in `lapismicprobe=1` preparation performs only codec register reads,
once on the existing keys thread. It avoids AFE MMIO until the clock state
is established. It
must first be compiled and validated on-device; it does not enable the ADC,
create `/dev/hakios-mic`, or prove microphone capture.

The combined diagnostic image arms a single capture request with
`lapismiccapture=1`. Holding both volume keys for about one second queues the
request. The AFE owner services it while playback is idle. It configures the
MT6369 and VUL9, checks DMA cursor bounds, reports per-channel peak and nonzero
counts, and restores prior settings. The first ringtone playback can exercise
the separate DL0-to-VUL1 capture check in the same boot. If the codec or another
engine is busy, or power or register verification fails, the log identifies
the stage. A restoration failure stops the audio worker for that boot. This
image exercises microphone hardware in the kernel. Normal iOS app routing
through `/dev/hakios-mic` is a separate integration step.
