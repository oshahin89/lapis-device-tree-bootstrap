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

## Playback start, traced on Android (`device/captures/android_audio_regmap_trace2_2026-11-22.txt`)

Private tracing instance, events regmap_reg_write + clk_enable / set_rate /
set_parent, music started inside the window. Thread `writer` (audioserver),
594.354 .. 594.397 s, in order (topckgen = syscon 0x10000000, CLK_CFG
set/clr pairs, 0x8 = CLK_CFG_UPDATE):

1. infracfg 0x10001000 +0xc88 = 0x40; clk afe_audio_f26m_ck; AFE +0x10
   (AUDIO_TOP_CON?) = 0x361d (syscon view of the AFE); clk top_audio_h_sel.
2. AFE: 0x38 = 0x374, 0xba0 = 0x80070404, 0xbd4 / 0xc54 = 0x7fff0412,
   0x1204 = 0xb8, **I2S OUT4 block 0x13c0 = 0x109ff802, 0x13c4 = 0x80098465,
   0x13c8 = 0x0410c462, 0x13cc = 0x15f80000, 0x13d0 = 0x00a00100,
   0x13e4 = 0x1800**, I2S IN4 0x1580 = 0x109ff800, 0x1590 = 0x0a000445,
   0x1690 = 0x88b2a008, memif base/end 0x4624/0x4634 = 0x11059000/0x1105afff
   and 0x4df4/0x4e04 = 0x1105b000/0x1105cfff (AFE SRAM), 0x7848/0x784c/0x785c
   (HW SRC), 0x10/0x0/0x4/0x8/0xc = 0, 0x14 = 1.
3. memif 0x4640 = 0x2330068 -> 0x2330a68 -> 0x2330a69 (enable).
4. Clocks: apll2 on, top_aud_2_sel <- apll2_ck (CLK_CFG 0xc8/0xc4 0x100),
   top_aud_engen2_sel <- apll2_d4 (0xb8/0xb4), top_audio_h_sel <- apll2_ck
   (0xd8/0xd4 0x30000), update bits 0x4000 / 0x200 / 0x80000; afe_apll2_ck,
   afe_apll_tuner2; top_apll_SI1_m_sel, top_aud_1_sel, top_apll12_div_SI1
   (topckgen 0x320 = 0x23ffd: I2S MCK divider).
5. AFE 0x38 = 0x375, 0x14 = 9; **I2S OUT4 enable 0x13c0 = 0x109ff803**, I2S
   IN4 0x1580 = 0x109ff801.
6. Amps (6-0037 then 6-0034): reg 0x00 = 0xf250, 0xf258, 0xf250, 0xe0 = 1,
   0xe1 = 1; later 0x00 = 0xf258 both. (Only regmap writes; the container
   profile goes out through the driver's raw I2C path, not seen here.)
7. AFE 0x4640 -> 0x2030a09; interconnect 0x2f04 = 0x400000, 0x2f24 = 0x800000;
   memif 0x4df0/0x4e00 = 0, 0x4e10 = 0x100a48 -> 0x100a4a (DL_24CH on);
   0x2390/0x23b0, 0x1a0/0x1a4, 0x1c0/0x1c4, 0x3704/0x3724, 0x7854, 0xba0;
   memif 0x51c4/0x51d4 = 0x1105d000/0x1105d9ff; HW SRC 0x7840 + 80
   coefficient writes to 0x7868; 0x2598/0x25b8; 0xba0 = 0x70485; IRQ 0x210/0x214.

Not in the trace: the audio power domain (scpsys, done at pm_runtime before),
APLL2 PLL registers (apmixedsys, not regmap), and the DSP's own AFE writes.

**I2S4 pins** (AFE pinctrl, pinmux = pin << 8 | mode): I2SIN4 GPIO 42, 43,
45 mode 2 (BCK, LRCK, DI), I2SOUT4 data GPIO 44 mode 2; off = mode 0.

## Amp configuration

- I2C trace on adapter 6 during the first playback after a reboot
  (`device/captures/android_i2c6_playback_start_2026-11-22.txt`): the driver
  only reads status and writes reg 0x00 = 0xf250, 0xf258, 0xf250, 0xe0 = 1,
  0xe1 = 1, later 0x00 = 0xf258. The profile is loaded at boot, not per stream.
- The `rw` sysfs node always reads from register 0 (a 256-register dump came
  back 0xf258 everywhere), so cloning like the vibrator does not work.
- The container decodes cleanly instead: the RCV / SPK device lists (0x314..,
  0x409..) are 39 identical bitfield writes `{value u16, bitfield u16}`,
  bitfield = reg << 8 | pos << 4 | (len - 1) (e.g. 0x20c3 = reg 0x20 [15:12] = 2);
  `tools/lapis_shim/tfa9865_init.h`. The "music" profile adds a few more
  (0x2130 = 1, reg 0 bit 4, 0x2030 = 0, 0x7ce1 = 1, 0x50f0, 0x5b41 ...).

## taud v1 on the phone (dual_v11, 2026-11-22): the AFE is reachable

After a reboot from Android the audio side keeps Android's state:
- audio PWR_CON 0x1c001e18 = 0xc200000d (both acks, ON, ON_2ND, RST_B; ISO and
  CLK_DIS clear): **already powered**; SRAM ack clear, bus protect clear,
  peri audio gates 0x18 = 0 (open).
- topckgen keeps Android's audio muxes: 0xb0 = 0x03050602 (aud_engen2 = 2),
  0xc0 = 0x0d040101 (aud_2 = apll2), 0xd0 = 0x0203000d (audio_h = 3), 0x320 =
  0x3fff.
- **APLL2 is off**: CON0 0x34c = 0x84000000 (en bit 0 clear), CON3 0x358 =
  0x80000002 (ISO set, PWR_ON clear); PCW 0x350 = 0x78fd5264 (x 26 MHz / 2^24
  / 16 = 196.608 MHz, the 48 kHz family). APLL1 CON0 0x338 = 0x84000000.
- AFE reads fine: 0x00 = 0x80, 0x04 = 0x550000, 0x10 = 0x361f, 0x14 = 0,
  0x38 = 0, 0xba0 = 0, I2SOUT4 0x13c0 = 0x009ff800 / 0x13c4 = 0x800984a5,
  memif 0x4640 = 0x330068, DL_24CH 0x4e10 = 0x100048.

## Plan

1. Which amp is fitted (Android: bound I2C drivers, `/proc/asound`, firmware).
2. Read-only shim probe: amp ID over I2C (8-bit DATA_PORT stores, see
   lapis_next_session.md vibration notes), AFE / I2S register state.
3. Amp init (SIA9197 is register-programmed; TFA98xx needs its container
   firmware) + AFE memif DMA → I2S out → a test tone from the shim.
4. XNU driver + an IOAudio / CoreAudio-facing service (the big part).
