# lapis audio bring-up

Started 2026-11-22. Nothing plays yet; this is the hardware map.

## From the runtime device tree (`device/runtime_fdt.dtb`)

- **AFE** (audio front end, DMA + I2S engines): `mt6878-afe-pcm@11050000`,
  `mediatek,mt6878-sound`, 0x9000 bytes, IRQ 0x188, power domain, ~33 clocks
  (APLL1/2, audio muxes, I2S dividers).
- **Machine driver**: `mediatek,mt6878-mt6369-sound`: the MT6369 PMIC codec
  (earpiece, headset, mics) plus speaker amps on `mediatek,spk-i2s = <0x19 0x18>`
  (I2S out 25 / in 24, for the amps' echo reference).
- **Speaker amps** (two channels), all candidates on the I2C bus
  `i2c@11e01000` (mediatek,mt6989-i2c, 400 kHz). The DT lists several second
  sources; only one is fitted:
  - `si,sia91xx-i2c` at **0x6c** (left, "SpkrLeft") and **0x6f** (right),
    type `sia9197x`, reset GPIO 188 / 189, IRQ GPIO 31 / 41
    (`speaker-codec-sia`);
  - `tfa,tfa98xx` at **0x34** and **0x37**, same reset / IRQ GPIOs
    (`speaker-codec`);
  - `richtek,rt5512` at 0x5c.
- **Audio DSP**: `mediatek,snd-audio-dsp` (RV55 core, smart-PA processing);
  stock Android routes most playback through it. We aim to bypass it: AFE DMA
  → I2S → amp directly.

## Plan

1. Which amp is fitted (Android: bound I2C drivers, `/proc/asound`, firmware).
2. Read-only shim probe: amp ID over I2C (8-bit DATA_PORT stores, see
   lapis_next_session.md vibration notes), AFE / I2S register state.
3. Amp init (SIA9197 is register-programmed; TFA98xx needs its container
   firmware) + AFE memif DMA → I2S out → a test tone from the shim.
4. XNU driver + an IOAudio / CoreAudio-facing service (the big part).
