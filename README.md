# Nura on the Xiaomi Mi 10 (umi)

Port of Nura (formerly postmarketOS) to the Xiaomi Mi 10 5G, codename `umi`
(SM8250 / Snapdragon 865, A-only, UFS).

Milestone 1 is "a working image first": package the existing mainline bring-up
(`colorgarden/umi-mainline` on GitHub, commit `e38cb76b41c9`) for pmbootstrap
with as few changes as possible, then clean it up towards a pmaports merge
request.

## Status

| Item | State |
|---|---|
| Kernel patch series on linux 6.12.94 | applies cleanly with `patch -p1` |
| Device tree | compiles (clang 18), `xiaomi,umi`, msm-id `0x164 0x20001`, board-id `0x2b 0` |
| Added drivers (panel, ST FTS touch, fuel gauge, SMB5 charger, GPI fix) | compile (clang 18) |
| Kernel config | resolves; meets the pmaports `default` kconfig rules except BTF debug info |
| Full kernel build | builds in GitHub Actions (Alpine clang 23, about 70 min on 4 cores) |
| `pmbootstrap install` / `boot.img` | builds; header v2, gzip kernel, DTB and GPU firmware in place (checked by unpacking the image) |
| RAM boot on the phone (`fastboot boot`) | kernel boots, USB network + debug shell work, UFS detected, panel driver sends its on-sequence, ST FTS touch probes |
| Picture on the panel | **black so far**: panel starts at brightness 1/2047; the initramfs hook now raises it and re-sends the on-sequence (untested) |

Target hardware variant: CSOT panel (`j2_mp_42_02_0b`, hwlevel MP,
`oled_panel_id` 0B) with ST FTS touch. A Samsung-panel unit needs different
panel data (see `raizer88/umi-mainline-arch`); Goodix touch is unsupported.

## Layout

```
pmaports/device/testing/
  device-xiaomi-umi/     deviceinfo + device package
  linux-xiaomi-umi/      kernel 6.12.94 + 5 patches + config
  firmware-xiaomi-umi/   Adreno 650 firmware, also pulled into the initramfs
tools/
  make-series.sh         regenerates the patch series from an umi-mainline checkout
  kconfig-frag.py        pmaports kconfigcheck.toml -> config fragment / checker
.github/workflows/build.yml   builds kernel + boot.img online
  config-fragments/      the four fragments merged on top of arm64 defconfig, in order
```

## What differs from the bring-up

- Kernel command line is no longer forced in the kernel; it comes from
  `deviceinfo_kernel_cmdline` (same `*_ignore_unused` and SMMU switches).
- No Rust / QR-code panic screen (plain `kmsg` panic screen instead).
- `CHARGER_BQ25970` (40 W charge pump) is disabled until charging has been
  checked on the device. Fuel gauge and PM8150B charger stay enabled.
- Modules are zstd-compressed; pmaports kconfig rules are merged underneath
  the bring-up fragment.
- Only the ST FTS vendor-derived touch driver is included.

## Build and first boot

### Online (GitHub Actions)

`.github/workflows/build.yml` builds the kernel and `boot.img` on GitHub's
free arm64 runner, or on a faster provider named in the `RUNNER` repository
variable, on every push that touches `pmaports/`, or by hand from the
Actions tab ("Build boot image" > "Run workflow"). Download `boot-xiaomi-umi`
from the run's Artifacts, unzip it, then on any machine with `fastboot`:

```sh
fastboot boot boot.img                 # RAM only, nothing is flashed
```

### Locally

On a Linux machine with pmbootstrap 3.x and `fastboot`:

```sh
pmbootstrap init                       # once, so pmaports gets cloned
cp -r pmaports/device/testing/* "$(pmbootstrap config aports)/device/testing/"
pmbootstrap init                       # vendor xiaomi, device umi, pick a UI
pmbootstrap build --arch aarch64 linux-xiaomi-umi   # the long step
pmbootstrap install --add postmarketos-mkinitfs-hook-debug-shell
pmbootstrap flasher boot               # fastboot boot: RAM only, nothing is flashed
```

Phone in fastboot mode (power off, hold volume down + power). Expected on the
first boot: kernel log or splash on the screen and a debug shell reachable with
`telnet 172.16.42.1` over USB. Power-cycling returns to Android.

Bootloader quirks recorded by the bring-up: only one `fastboot boot` per
bootloader session (`fastboot reboot-bootloader` in between), and the kernel in
the boot image must be compressed.

Flashing the root filesystem (`pmbootstrap flasher flash_rootfs`) writes to
`userdata` and **erases all Android user data**. Do that only after the RAM
boot works, and after a backup.

## Known gaps and risks

- First boot log (17 Nov 2026) also shows: duplicate SPMI device registration
  warnings, Bluetooth firmware `qca/htbtfw20.tlv` not in the initramfs, and
  PCIe (Wi-Fi), Type-C, cpufreq and SD-card nodes stuck in deferred probe on
  `dtbo-stubs` suppliers. None of these block the shell.

- The "works" claims of the bring-up (display, touch, GPU, Wi-Fi, charging,
  Plasma Mobile) are theirs and have not been reproduced here.
- The charger and fuel-gauge drivers are third-party code that controls
  battery charging. Watch temperature and charge current on the first charge.
- ADSP/CDSP/SLPI/Venus firmware (`qcom/sm8250/umi/*.mbn`) is not packaged; it
  has to be extracted from the phone's own modem partition. Until then: no
  audio, sensors or hardware video decode.
- Modem/calls, camera, Bluetooth, NFC and fingerprint are not assessed.
- The device tree carries 200+ stub labels so the stock `dtbo` overlays still
  resolve, and boots with `clk_ignore_unused`, `pd_ignore_unused` and
  `regulator_ignore_unused`. None of this is acceptable upstream.
- pmaports builds with a newer clang than the one used for the compile checks.

## Next

1. Full build in pmbootstrap, RAM boot, collect `dmesg`.
2. Root filesystem on `userdata`, UI, Wi-Fi.
3. Firmware extraction script and `firmware-xiaomi-umi` subpackages for the DSPs.
4. Audio (UCM), sensors, Bluetooth.
5. Forward-port to the pmaports SM8250 kernel (7.2), drop the stubs and
   `*_ignore_unused`, own `dtbo`, then propose `device-xiaomi-umi` for testing.
