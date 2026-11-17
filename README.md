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
| Picture on the panel | **works**: the initramfs hook re-sends the panel on-sequence and sets brightness 1500; console + on-screen keyboard visible at boot (17 Nov 2026) |
| 90 Hz | **works**: panel TE and frame completions measured at 90/s while scrolling (kernel patch 0006, stock 90 Hz on-sequence) |
| Plasma Mobile on internal storage, Wi-Fi, Bluetooth | **work** (17 Nov 2026) |
| Booting from the `boot` partition | **works**: normal restarts go straight into Nura; from device package r3 on, kernel updates rewrite `boot` themselves (17 Nov 2026) |
| Sound: both loudspeakers | **work** through PulseAudio (ADSP firmware loaded from the phone's own modem partition, CS35L41 amps on Tertiary MI2S; 17 Nov 2026). Plasma also needs `xwayland`, which the workflow adds |
| Sound: stereo | top receiver (0x42) = left, bottom loudspeaker (0x40) = right, confirmed on the phone by setting the amps' slot registers by hand; kernel patch 0008 (pkgrel 4) sets it at stream start |
| Sound: internal microphones | **work** through PulseAudio as a stereo `Mic` input (top mic left, bottom mic right; 17 Nov 2026) |
| Modem (SDX55M, 4G/5G) | in progress: **boots to mission mode** on mainline (PCIe RC2, GIC MBI for its MSIs, Sahara image server `umi-modem`); network registration and data not done yet, see [docs/modem.md](docs/modem.md) |
| Sound: wired headsets (no 3.5 mm jack; USB-C analog adapters go through the WCD938x), earpiece-only output for calls | not working yet |

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

### Installing to the phone

With a full image (`full_image` in the workflow, artifact `xiaomi-umi-<ui>`):

```sh
fastboot flash userdata xiaomi-umi.img   # erases Android user data
fastboot flash boot boot.img
fastboot boot boot.img                   # not "fastboot reboot", see below
```

Right after flashing, `fastboot reboot` drops back into fastboot instead of
starting the new image (bring-up finding), so start it with `fastboot boot`.
From then on, normal restarts boot Nura from the `boot` partition, and kernel
updates installed with `apk` write the new boot image there themselves
(`deviceinfo_flash_kernel_on_update`).

Updating from a CI run without reflashing: copy the `.apk` files from the
artifact's `packages/` folder to the phone and run
`sudo apk add --allow-untrusted ./*.apk`.

## Known gaps and risks

- The stock bootloader applies the stock `dtbo` and overwrites the device
  tree's root `model` and `compatible` with `qcom,kona-mtp`/`qcom,kona`.
  Anything matching the root on `qcom,sm8250` or `xiaomi,umi` misses; patch
  0010 adds `qcom,kona` where it mattered (pd-mapper, cpufreq-dt blocklist).

- First boot log (17 Nov 2026) also shows: duplicate SPMI device registration
  warnings, and PCIe (Wi-Fi), Type-C, cpufreq and SD-card nodes stuck in deferred probe on
  `dtbo-stubs` suppliers. None of these block the shell.

- The "works" claims of the bring-up (display, touch, GPU, Wi-Fi, charging,
  Plasma Mobile) are theirs and have not been reproduced here.
- The charger and fuel-gauge drivers are third-party code that controls
  battery charging. Watch temperature and charge current on the first charge.
- DSP firmware is not packaged. `msm-firmware-loader` loads the ADSP firmware
  (audio) from the phone's own modem partition at boot. CDSP, SLPI (sensors)
  and Venus (video decode) are not set up yet.
- The stock bootloader applies the stock `dtbo` and replaces the root
  `compatible` with `qcom,kona-mtp`/`qcom,kona`; drivers that match on the SoC
  compatible need a `qcom,kona` entry (kernel patch 0010).
- Plasma Mobile with PulseAudio hangs without `xwayland`: KWin still sets
  `DISPLAY`, and PulseAudio's X11 autostart waits for it forever (no sound,
  volume popup every few seconds). The workflow installs `xwayland`; for a local
  build run `pmbootstrap config extra_packages xwayland` first.
- Modem/calls: not working yet; see [docs/modem.md](docs/modem.md).
- Camera, NFC and fingerprint are not assessed.
- The device tree carries 200+ stub labels so the stock `dtbo` overlays still
  resolve, and boots with `clk_ignore_unused`, `pd_ignore_unused` and
  `regulator_ignore_unused`. None of this is acceptable upstream.
- pmaports builds with a newer clang than the one used for the compile checks.

## Next

1. Full build in pmbootstrap, RAM boot, collect `dmesg`.
2. Root filesystem on `userdata`, UI, Wi-Fi.
3. Sound: USB-C analog headset adapters, earpiece-only output for calls (with the modem).
4. Sensors (SLPI), video decode (Venus).
5. Forward-port to the pmaports SM8250 kernel (7.2), drop the stubs and
   `*_ignore_unused`, own `dtbo`, then propose `device-xiaomi-umi` for testing.
