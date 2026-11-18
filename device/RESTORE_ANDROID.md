# Back to stock Android (lapis, slot b)

Use this whenever the phone is on an iOS / test image and you want Android back.
It only rewrites `boot_b`; Android data, root (Magisk `init_boot`), `gz` and the
`iosroot` partition are left as they are.

1. Put the phone in fastboot: hold **Power + Vol-down** until the fastboot screen.
2. On the PC, in the project folder:

```bash
cd /home/oshahin/lapis-device-tree-bootstrap
```

```bash
fastboot devices
```

```bash
fastboot flash boot_b device/hyperos4_readback/boot.img
```

```bash
fastboot --set-active=b
```

```bash
fastboot reboot
```

Android boots in about a minute.

## If something else was changed

- `gz_b` (only if a GenieZone test was flashed):

```bash
fastboot flash gz_b device/hyperos4_readback/gz.img
```

- Un-root (stock `init_boot_b`, only if wanted):

```bash
fastboot flash init_boot_b device/hyperos4_readback/init_boot.img
```

Do not take OTA updates while `iosroot` exists (LK may rewrite the GPT).
