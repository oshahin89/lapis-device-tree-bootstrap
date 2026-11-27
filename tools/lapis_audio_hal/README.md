# HakiOS CoreAudio reference build

These files are based on the HakiOS `main` branch at
commit `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`, under the upstream MIT
terms in `LICENSE.upstream`. They are built as a compatibility investigation
for Lapis's iOS 16 user space. Optional discovery and route marker ioctls are
disabled in the ordinary build.

The plug-in expects `/dev/hakios-audio` and VirtualAudio routing. On 2026-11-27,
a reversible phone experiment connected these to Lapis's PCM speaker path;
the user heard system audio. Logs showed repeated streams, no reported dropped
frames and no I2C errors in that run. The experimental kernel scales output to
500/32768 of input amplitude, so it is intentionally quiet. Long-term playback,
full volume controls, and persistence across boots remain to be validated on
the phone. `tools/lapis_audio_probe/system-audio-tested.gz` preserves the exact
signed plug-in from the successful experiment for byte-checked installation
and cleanup.
