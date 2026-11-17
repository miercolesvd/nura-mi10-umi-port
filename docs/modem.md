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

## Approach on mainline

| Piece | Where | State |
|---|---|---|
| PCIe RC2 + PHY, AP2MDM_STATUS high / ERRFATAL low from boot | kernel patch 0011 (pins as on the Sony Xperia 1 II/5 II) | built, untested |
| SBL over BHI | `mhi_pci_generic` (generic 17cb:0306 entry), `qcom/sdx55m/sbl1.mbn` linked into the firmware path by `umi-modem-start` | built, untested |
| SAHARA (2/3, SBL) and EFS (10/11, mission mode) channels | kernel patch 0012: SDX55 channel table + `mhi_sahara_cdev` (`/dev/mhi_sahara`, `/dev/mhi_efs`; a write is one chained transfer, as SBL needs) | built, untested |
| Image server + EFS syncs in RAM | `umi-modem` package: `mi10-mdm -M`, ported from mi-10-hotspot (same Sahara/EFS code, its simulator tests pass) | built, untested |
| QRTR name service | mainline's in-kernel `ns` accepts node 2 announcing node 3's services, and the router learns the route to node 3 from those announcements: no patch needed | to verify |
| Carrier configs over QRTR TFTP | `tqftpserv` | to do |
| Data call | ModemManager over QRTR, or `mi10-qmi` from mi-10-hotspot; data on `mhi_net` (IP_HW0) with rmnet/QMAP | to do |

`umi-modem.service` is not enabled by default while this is being tested.
`mhi_pci_generic` is blacklisted so the modem only starts once its Sahara
server is ready; the service loads it (or rebinds the device on restart).

Open question: if this modem reports subsystem ID 17cb:010c, mainline maps it
to the Foxconn T99W175 entry, which has no Sahara channel. `lspci -nnv` on the
phone tells.

## Safety

Firmware is read from the modem partition mounted read-only by
msm-firmware-loader. The NV partitions `mdm1m9kefs1..3` are only opened
read-only; the modem's EFS syncs are kept in `/run/umi-modem/efs` (RAM).
Never write to `mdm1m9kefs*`, `modemst*`, `fsg` or `persist`.

Calls also need the voice path (q6voice), not done yet.
