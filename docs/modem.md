# Modem (SDX55M) on mainline: notes and plan

Status: **not working**. The Mi 10's modem is a separate Qualcomm SDX55M chip
on PCIe, not part of the SM8250. It has no flash of its own for its software:
the phone loads it over PCIe at every boot.

The userspace half of this is solved, on the stock LineageOS 4.19 kernel, by
[mi-10-hotspot](https://github.com/miercolesv/mi-10-hotspot)
(`docs/secret-sauce.md`): boot image map, EFS sync, QRTR relay, QMI data call.
This file maps that onto the mainline kernel used here.

## Facts from the stock device tree

From LineageOS 23.2 umi `boot.img` (base `kona v2.1` DTB) and `dtbo.img`
(entry 11, `xiaomi umi`, board-id 0x2b), build 2026-11-15. The umi overlay does
not touch the modem node.

Modem control node `/soc/qcom,mdm0`, `compatible = "qcom,ext-sdx55m"`:

| Signal | TLMM GPIO |
|---|---|
| `mdm2ap-errfatal` (input, IRQ) | 1 |
| `mdm2ap-status` (input, IRQ) | 3 |
| `ap2mdm-status` (output) | 56 |
| `ap2mdm-errfatal` (output) | 57 |

- `qcom,mdm-link-info = "0306_02.01.00"`: PCIe device 17cb:0306 on RC2.
- No soft-reset or PON GPIO in either the base DTB or the umi overlay.
- Pinctrl states `ap2mdm_active` / `mdm2ap_active` (and `_sleep`).

PCIe RC2 (`qcom,pcie@1c10000`, downstream `pci-msm`):

| Item | Value |
|---|---|
| PERST# | TLMM GPIO 85 |
| WAKE# | TLMM GPIO 87 |
| `qcom,boot-option = 1` | not enumerated at boot; the link comes up when the modem is powered |
| IOMMU | SID base 0x1d00 |

## What mainline 6.12 already has

- `mhi_pci_generic` knows 17cb:0306 as `qcom-sdx55m` and loads
  `qcom/sdx55m/sbl1.mbn` over BHI by itself (the hotspot's step 1).
- `qrtr-mhi` on channel 20/21 `IPCR`.
- `mhi_net` on channels 100/101 `IP_HW0` (rmnet/QMAP data).
- ModemManager and libqmi can talk QMI over QRTR.

## What is missing

1. **Power and link bring-up.** Downstream `esoc` drives GPIO 56/57, waits for
   GPIO 3, then trains the PCIe link. Mainline has no esoc. Options: a small
   driver or `pci-pwrctrl`-style sequencing on the RC2 node, or a userspace
   helper that toggles the GPIOs and triggers a PCI rescan.
2. **Sahara (channel 2/3) and EFS (channel 10/11)** are not in mainline's
   SDX55 channel table, and nothing exposes them to userspace. Needs a kernel
   patch (channel table plus a char device, for example via `mhi_wwan_ctrl`).
3. **QRTR name service relay**: the kernel's in-kernel name service likely has
   the same "node 2 announces node 3's services" check that the hotspot
   patches in Linaro's `ns.c`. To verify and patch.
4. **Userspace**: port `mi10-mdm` (Sahara image server plus EFS sync kept in
   RAM) from esoc ioctls to the new interfaces; `tqftpserv` for carrier configs;
   then ModemManager.
5. **Firmware**: `image/sdx55m/` on the phone's `modem` partition, plus the raw
   `mdm1m9kefs1/2/3` partitions served read-only. Never write to the modem's NV
   partitions.

Calls also need audio (ADSP), which is not done yet.
