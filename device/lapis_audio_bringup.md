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

## Fitted on this phone (Android, 2026-11-22)

`device/captures/android_i2c_audio_2026-11-22.txt`: **TFA98xx at 6-0034 and
6-0037** (driver tfa98xx), firmware `/vendor/firmware/tfa98xx.cnt`, one ALSA
card `mt6878-mt6369`. I2C6 = i2c@11e01000 (also gate-ic rt4831 0x11, wl2866d
0x28, typec-audioswitch 0x42). `tools/lapis_shim/tamp.c` (dual_v9) reads the
amps' revision registers.

## Amps answer (tamp v2 on the phone, 2026-11-22)

- I2C6 pins SCL 137 / SDA 138 are already mode 1. LK leaves CONTROL = 0: no
  ACK-error detection, so v1 saw 0xffff "complete" for absent devices. With
  CONTROL |= 0x20 (ACKERR_DET): before reset release 0x28 (wl2866d) and 0x42
  (typec switch) answer; 0x11 and 0x75 do not.
- Amp reset GPIO 188 / 189: LK leaves them input, low = in reset
  (tfa98xx reset-polarity 0). Driven high (mode 0, output 1, 5 ms): **both
  amps answer**. 0x34 and 0x37 alike: REV 0x03 = 0x9866, 0x00 = 0xf261,
  0x01 = 0x0080, 0x10 = 0x7e8f, 0x13 = 0x0000.
- `device/tfa98xx_hyperos4.cnt` (1532 B, container "PM3_01", customer "Mi",
  application "0b65_08"): devices RCV / SPK, patch `TFA9865MN2B0_dummy.patch`
  (no DSP code), profiles music / voip / voice / rec / left / right /
  standby / powerdown, as bitfield writes.

## Android mixer routing (tinymix, `device/captures/android_audio_mixer_2026-11-22.txt`)

- Speakers: **I2SOUT4_CH1/CH2 <- DL_24CH_CH1/CH2** (the ADSP playback path,
  `adsp_playback_sharemem_scenario` On), `I2SOUT4_HD_Mux` = Low_Jitter,
  I2SOUT4 2 channels. Echo reference: I2SIN4_CH1/2 -> UL3.
- HW_SRC_0 takes DL4 + DL_24CH -> UL_CM0 (ADSP side).
- For the shim we bypass the ADSP: an AP memif (DL0..DL8) -> I2SOUT4 through
  the same interconnect.
- debugfs (`mtksocaudio`) is not there on this user build; next capture is
  the regmap write trace during playback start.

## Plan

1. Which amp is fitted (Android: bound I2C drivers, `/proc/asound`, firmware).
2. Read-only shim probe: amp ID over I2C (8-bit DATA_PORT stores, see
   lapis_next_session.md vibration notes), AFE / I2S register state.
3. Amp init (SIA9197 is register-programmed; TFA98xx needs its container
   firmware) + AFE memif DMA → I2S out → a test tone from the shim.
4. XNU driver + an IOAudio / CoreAudio-facing service (the big part).
