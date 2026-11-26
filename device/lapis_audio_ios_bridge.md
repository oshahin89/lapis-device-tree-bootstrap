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
on Lapis. The original failure and the availability of the required routing
data on this particular image must be investigated before changing the live root.

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
