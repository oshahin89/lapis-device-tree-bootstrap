# Lapis battery and charging bring-up (2026-11-22)

## Hardware (Android, read-only)

| Part | Where | Notes |
|---|---|---|
| Fuel gauge | TI **bq28z610**, I2C0 `0x11c20000` (`mediatek,mt6989-i2c`, 400 kHz), address `0x55` | Android: 86 %, 4245 mV, +671 mA charging, 29.1 °C, full 6086 / design 6580 mAh, 168 cycles |
| Main charger | MT6375 on I2C5 `0x11280000`, address `0x34` | not used yet: the gauge's current and status say whether it charges |

bq28z610 standard commands (2 bytes, little endian): `0x06` temperature
(0.1 K), `0x08` voltage (mV), `0x0A` BatteryStatus (bit 5 FC, bit 6 DSG),
`0x0C` current (mA, signed, + = charging), `0x10`/`0x12` remaining / full
charge capacity (mAh), `0x2A` cycle count, `0x2C` state of charge (%).

## I2C controller

Stock `i2c-mt65xx.ko`: `mt6989_compat` → `mt_i2c_regs_v2` (upstream
offsets: SLAVE_ADDR `0x94`, CONTROL `0x10`, TRANSFER_LEN `0x14`, TRANSAC_LEN
`0x18`, START `0x24`, EXT_CONF `0x28`, FIFO_ADDR_CLR `0x38`, TRANSFER_LEN_AUX
`0x44`, FIFO_STAT `0xf4`), flags `aux_len_reg`, `auto_restart`. LK leaves
I2C0 configured (CONTROL `0x38`, TIMING `0x20`).

`tools/lapis_shim/tgauge.c` on the phone: v1 (`9de3fe75…`) — 0 errors, but
values one byte off (SOC `5400`, voltage `C000`). v2 (`dfadebdd…`) raw FIFO:
voltage `00 BD 10 00`, SOC `00 54 00 ..`, FIFO count = requested + 1 — the
FIFO holds **one extra leading byte**; skip it, then little endian
(`0x10BD` = 4285 mV, `0x54` = 84 %).

## Driver (v16)

`device/xnu-lapis/driver/sweet_lapis_battery.c` (+ patch/stage 0088): WRRD
gauge reads (skip the leading FIFO byte when the count is n + 1, 3 retries,
plausibility checks); SweetBattery gets a lapis source (default on,
`lapisbattery=0` disables): SOC, voltage, current, temperature, cycle count,
full capacity from the gauge; charging = current > 10 mA, external =
charging or not DSG, full = FC or 100 %; polled every 10 s. A failed read
keeps the last reading (static 100 % before the first).

**v16 on the phone** (`080e04aa…`, CI 45027480867): iOS shows **86 %** from
the gauge; log `SWEET BATTERY: lapis bq28z610 gauge, 86% 4305mV external=1
charging=1`. With the charger plugged in, iOS did not show charging. v17
logs every external/charging change (and current steps > 200 mA) and
publishes `AdapterDetails` (18 W, FamilyCode `0xe0004004` dedicated USB
charging port) while external, in case powerd/BatteryCenter need it.

**v17 on the phone** (`81fcf95d…`, CI 45029405768): the charging icon now
appears (AdapterDetails) but comes and goes. Log: with the charger in, the
gauge current swings between about +114 mA and −538 mA (BatteryStatus mostly
`0x40` DSG) — the charger supplies only a few hundred mA, about the system
load, so the net current hovers around zero and "external" (derived from the
current) flips. Under Android the charger driver programs the MT6375 (I2C5
`0x34`) for ~3 A; under iOS nobody does. Next: read VBUS / power-good from
the MT6375 for "external" (read-only), then raise its input-current limit
and charge current to moderate values (writes; registers from the stock
`mt6375-charger` module).

## MT6375 charger (tchg, 2026-11-22)

Stock `mt6375.ko`: 16-bit register = bank << 8 | reg, bank → I2C address:
**0 = 0x4E, 1 = 0x34 (the DT client), 2 = 0x1A, 3 = 0x4A, 4 = 0x64, 5 = 0x3F**
(probe stores `[client+8]` = 0x4E, `[+16]` = the original 0x34); vendor id
at 0x100 must be `0x7X`. tchg v1 read 0x4E (bank 0) by mistake (zeros).
Stock `mt6375-charger.ko` fields: CHG_EN/BUCK_EN/HZ 0x120 b0/b1/b3, IAICR
0x122 [6:0] (100–3225 mA, step 25, offset 2), CV 0x125 [6:0] (3900–4710 mV,
step 10), CC 0x126 [5:0] (300–3150 mA, step 50, offset 6), WDT 0x12a
([1:0], RST b2, EN b3), IC_STAT 0x134, PORT_STAT 0x151, ST_PWR_RDY 0x1e0 b0.

tchg v2 (`db4afa70…`) on the phone, charger in: vid `72`, CTRL `33`
(charging, buck on), **AICR `12` = 500 mA, CC `0A` = 500 mA**, WDT `01`
(watchdog off), IC_STAT 4, PWR_RDY `C1` (bit 0 = 1). The single-byte
reads here had no extra FIFO byte (count 1).

**v18 driver:** `sweet_lapis_battery.c` also manages the MT6375 (I2C5 0x34):
"external" = PWR_RDY; while plugged and 5–45 °C, IAICR and CC → 1500 mA
(`0x3A` / `0x1E`), only those bits, each write read back, any mismatch
stops all charger writes (`lapischarger=2` read-only, `=0` off). The FIFO
skip is now "count = n + 1".
