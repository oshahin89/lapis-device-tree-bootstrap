# Lapis iOS audio bridge

## Evidence (2026-11-26)

The CPU-rendered boot reached a complete, touch-responsive UI according to the
device user. Its decoded log (`/tmp/sweetlog_cpu_audio.txt`) reports
`lapisgpu=0`, `lapismetal=0`, mediaserverd starting, and the kernel audio
chime running with no reported I2C errors. The chime is not an iOS audio
stream: `sweet_lapis_audio.c` writes a fixed waveform to AFE SRAM and stops.

The root image preparation disables `VirtualAudio.plugin` because its inherited
script reports a mediaserverd crash (`tools/ios-sweet/sweet-root-tweaks.py`).
That note originates in the ios-sweet baseline; it is not evidence of a crash
on Lapis. Baseline `NOTES.md` line 70 attributes the prior crash to a missing
default VirtualAudio device during `ApplyBeamFormerPreference`. This suggests
that providing a suitable HAL device could help, but does not establish that
the upstream plug-in resolves it on this image. The availability of routing
data must also be checked before changing the live root.

## Upstream reference

HakiOS common commit `62e09f850f1f713192228cb9031da256033ede93` provides
`components/audio/audio_plugin.c`, a CoreAudio HAL plug-in, and a character
device implementation in `drivers/board/audio.c`. The plug-in offers a
`Speaker` device and writes 48 kHz signed little-endian stereo 16-bit PCM
frames to `/dev/hakios-audio`; writes must not block the audio thread. Its
comments describe using VirtualAudio for the built-in speaker route. This is
upstream's design, not evidence that it works with this iOS 16 image.

Lapis's tone path already uses stereo signed 16-bit samples at 48 kHz, but it
does not provide a streaming interface, flow control, or iOS routing. The
upstream Qualcomm audio sink is hardware-specific and must not be copied as
the MediaTek hardware driver.

## Work remaining

1. Recover the original VirtualAudio failure evidence and inspect this image's
   available plug-ins and routing data without modifying the active iOS root.
2. Design a bounded, nonblocking PCM queue and device interface. Account for
   hardware ring read position, wraparound, underruns, start/stop, synchronization,
   cache visibility and safe amplifier volume; never overwrite samples being
   played. Prove the interface with a controlled PCM source before attaching HAL.
3. Adapt and build the upstream HAL plug-in for this image and verify that
   mediaserverd loads it and exposes the route. Keep a reversible root-image
   deployment and gather crash logs if VirtualAudio fails.
4. Exercise playback from an iOS application and verify start, stop, volume,
   silence and repeated playback on the actual phone.

The GPU and Metal advertisement are disabled in the current phone boot image.

## PCM ingress probe (2026-11-26)

Kernel CI run `45827754715` built the opt-in `lapisaudioprobe=1` character
device. On the phone, the CPU-rendered UI and touch worked, and sweetlog
reported `/dev/lapis-audio-probe` created with the GPU disabled. The separate
`lapis_audio_probe` program wrote 480 stereo frames (960 nonzero samples);
the device's close log reported exactly those counts. The program attempts to
add a marker frame if its partial-frame rejection check fails; the count is
consistent with rejection, though it does not independently prove the marker
write succeeded if needed. This only verifies synthetic PCM ingress; it does
not exercise CoreAudio or play sound.

The original `IOMFB_FDR_Loader` was restored after the experiment and its
slot verified as original. The installed dual boot image still enables the
ingress probe; `lapisgpu=0` and `lapismetal=0` remain in its boot arguments.

## iOS-to-speaker packet probe (2026-11-26)

With `lapisaudioplayprobe=1`, the kernel waits for one 480-frame PCM packet
from the iOS test program, clips it to a conservative fixed level, repeats it
in the AFE SRAM ring and plays it for 300 ms. The user heard a short tone;
sweetlog reports the 480-frame write, then receipt of the packet and a moving
DL0 pointer with zero I2C errors. This demonstrates the test program's PCM
reached the speaker path, not continuous application playback or CoreAudio.
After the test the original FDR loader and the previously booted CPU-rendered
ingress-probe image were restored and checked by hash.

## Continuous PCM queue preparation

`device/xnu-lapis/driver/lapis_audio_queue.h` provides an 8192-frame bounded
stereo queue for a caller that supplies its own lock. Its host-side check
`tools/lapis_audio_probe/queue_check.c` exercises ordering, wraparound,
overflow and zero-filled underruns; it passed with address and undefined
behavior sanitizers on 2026-11-26. It is not yet connected to the kernel,
hardware ring, HAL or application playback. A real-time worker must use the
DL0 hardware read position safely, with an explicit guard and a policy for a
late worker before any continuous playback experiment.

## Short continuous-stream experiment (2026-11-26)

Build `1f5305e` was installed from the mobile bundle with
`lapisaudioprobe=1 lapisaudioplayprobe=1 lapisaudiostream=1` and the GPU
disabled. The user heard two different tones. The preserved sweetlog reported
72,000 frames accepted, zero dropped, 72,000 fed to the hardware ring, and
59,900 silent frames; the last includes silence after the short source ends.
This is one controlled test, not a measurement of long-term stability or proof
that application audio works. The user must restore the original FDR loader
and the known-good CPU image using the bundle's `restore.sh` after the test.

The same boot's normal shutdown reached filesystem unmount and `CPU halted`,
then panicked with `Halt/Restart Timed Out` at `IOPlatformExpert.cpp:884`.
The user returned to Android with hardware buttons. The log establishes a
shutdown timeout; the particular missing or failing platform power-off
mechanism is not yet confirmed.

## HakiOS HAL compatibility (2026-11-26)

On the discard-only `lapisaudiohalprobe=1` boot, the FDR test using HakiOS
HAL code opened `/dev/hakios-audio`, set muted volume (384 eighth-dB below
maximum), and wrote four stereo frames; sweetlog recorded all four frames
and eight nonzero samples. The user restored the original FDR and known-good
CPU image afterward. This exercises the HAL functions in a test executable;
it does not prove mediaserverd loads the plug-in or that apps can play audio.

The HakiOS HAL opens its sink during initialization and retains that file
descriptor across start/stop cycles. Therefore a permanent Lapis sink cannot
use `open` and `close` alone as playback start and stop signals. It must track
incoming frames, safely stop after inactivity, restart for subsequent writes,
honor mute and volume, and handle delays without overwriting the hardware DMA
reader. The current bounded experiment runs for 2.5 seconds only and is not a
permanent sink. The separate `lapisaudiohalprobe=2` experiment exercises the
actual HAL write path and a software mute against the speaker, and remains
opt-in and untested on the device at the time of writing.
