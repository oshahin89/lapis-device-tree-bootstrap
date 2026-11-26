# HakiOS CoreAudio reference build

These files are copied without modification from the HakiOS `main` branch at
commit `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`, under the upstream MIT
terms in `LICENSE.upstream`. They are built as a compatibility investigation
for Lapis's iOS 16 user space.

The plug-in expects `/dev/hakios-audio` and VirtualAudio routing. Neither is
currently connected to Lapis's experimental PCM output. Do not install this
bundle into the live iOS root until a working device interface, routing and a
reversible deployment have been established.
