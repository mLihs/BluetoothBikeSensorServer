# Wahoo KICKR (2017) — nRF Connect dump

- **Device name:** Wahoo KICKR 7D0B  
- **Address:** `F0:08:C6:D5:5C:DE`  
- **Hardware:** KICKR ’17 (user)  
- **Firmware:** v3.4.68 (user; Device Information read started in the log
  but was truncated before Firmware Revision `0x2A26`)  
- **Captured:** 2026-11-14 (~10:22 local)  
- **Tool:** nRF Connect (Android log export)  
- **Raw log:** `Wahoo_KICKR_7D0B_2017_nrf_connect.txt`

## Services discovered

| Service | UUID | Notes |
|---|---|---|
| Generic Access | `0x1800` | Device Name = `Wahoo KICKR 7D0B` |
| Generic Attribute | `0x1801` | Service Changed |
| Battery | `0x180F` | Battery Level |
| Device Information | `0x180A` | Manufacturer / Serial / HW / FW (values mostly unread) |
| Wahoo proprietary | `a026ee01-…` | Two chars (notify / write-no-resp) |
| Wahoo proprietary | `a026ee03-…` | One char (notify / write-no-resp) |
| **Cycling Power** | `0x1818` | Standard CPS |
| Wahoo proprietary | `a026ee0b-…` | One char |
| Wahoo proprietary | `a026ee06-…` | One char |

**Not present:** Fitness Machine / FTMS (`0x1826`), CSC (`0x1816`), Heart Rate.

## Cycling Power (`0x1818`)

| Characteristic | Props |
|---|---|
| Cycling Power Measurement `0x2A63` | Notify |
| Cycling Power Feature `0x2A65` | Read (value not in log) |
| Sensor Location `0x2A5D` | Read |
| Wahoo unknown `a026e005-…` | Indicate + Write |
| Cycling Power Control Point `0x2A66` | Indicate + Write |

No crank/cadence fields are expected from this trainer in practice: **Wahoo
KICKR does not provide cadence** (maintainer, 2026-11-14). Homewind / Zwift
need a separate CSC sensor (or a power meter that reports cadence) when the
only bike source is a KICKR.

## Finding for the FTMS proxy design (§2)

**This KICKR ’17 (FW 3.4.68) does not advertise FTMS.** Controllable ERG /
simulation on this unit would go through **Wahoo proprietary** services
(`a026ee…`), not FTMS Control Point forwarding — unless a newer firmware
exposes `0x1826` (**verify** on a post-2020 KICKR / current FW dump).

Read-only power for Zwift via Homewind: CPS (`0x1818`) already works as a
PWR slot. Cadence: not from this trainer.

## Follow-ups

1. Finish Device Information reads (FW string in the log file).
2. Read Cycling Power Feature bits (confirm no crank revolution data).
3. Dump a **newer** KICKR (or Core/Snap) known to speak FTMS for §2.
4. Document Wahoo proprietary control only if §2 deliberately targets
   non-FTMS Wahoo (out of scope vs. standard FTMS).
