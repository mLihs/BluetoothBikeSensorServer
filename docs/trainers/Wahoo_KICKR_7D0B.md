# Wahoo KICKR 7D0B — nRF Connect dump

- **Advertised name:** Wahoo KICKR 7D0B  
- **Address:** `F0:08:C6:D5:5C:DE`  
- **Firmware:** `3.4.68` (from Wahoo / device; DIS `0x2A26` not in truncated
  nRF “Read all” export)  
- **Captured:** 2026-11-14 (~10:22 local)  
- **Tool:** nRF Connect (Android-style log; file
  `nRFConnectWahhoo_Kicker.txt` / saved as
  `Wahoo_KICKR_7D0B_nrf_connect.txt`)  
- **Scope:** full GATT service discovery after connect; “Reading all
  characteristics” started but the export cuts off after Device Name — so
  Feature / DIS strings / battery values are **not** in this log.

## Services discovered

| Service | UUID | Notes |
|---|---|---|
| Generic Access | `0x1800` | Name, Appearance, PPCP, Central Address Resolution |
| Generic Attribute | `0x1801` | Service Changed |
| Battery | `0x180F` | Battery Level notify+read |
| Device Information | `0x180A` | Manufacturer, Serial, Hardware, Firmware (read; values not captured) |
| Wahoo proprietary | `a026ee01-0a7d-4ab3-97fa-f1500f9feb8b` | Wahoo custom (2 chars) |
| Wahoo proprietary | `a026ee03-0a7d-4ab3-97fa-f1500f9feb8b` | Wahoo custom |
| **Cycling Power** | `0x1818` | Standard CPS |
| Wahoo proprietary | `a026ee0b-0a7d-4ab3-97fa-f1500f9feb8b` | Wahoo custom |
| Wahoo proprietary | `a026ee06-0a7d-4ab3-97fa-f1500f9feb8b` | Wahoo custom |

**Not present in this dump**
- Fitness Machine / FTMS `0x1826`
- Cycling Speed and Cadence `0x1816`
- Tacx FE-C `6E40FEC1-…`

## Cycling Power (`0x1818`)

| Characteristic | Properties |
|---|---|
| Cycling Power Measurement `0x2A63` | Notify |
| Cycling Power Feature `0x2A65` | Read |
| Sensor Location `0x2A5D` | Read |
| Wahoo char `a026e005-…` (inside CPS service) | Indicate + Write |
| Cycling Power Control Point `0x2A66` | Indicate + Write |

## Device name (read)

`Wahoo KICKR 7D0B` (from `0x2A00`).

## Finding for the FTMS proxy design (§2)

On this connection, the KICKR exposes **standard CPS** plus several **Wahoo
proprietary** services (`a026ee…`), but **no FTMS (`0x1826`)**.

That differs from the backlog assumption that KICKR/Core always offers FTMS
for a 1:1 Control Point proxy. Possible explanations (**verify**):

1. Firmware / product variant without FTMS GATT (control only via Wahoo
   protocol or ANT+).
2. FTMS enabled only in another mode or after a Wahoo app handshake.
3. Incomplete discovery (unlikely here — the service list looks complete).

**For Homewind today:** this unit can already sit in the **PWR** slot as a
read-only power source (CPS), same path as Assioma/NEO power. Controllable
proxy needs either FTMS on a dump that actually shows `0x1826`, or a Wahoo
proprietary client (out of scope unless designed).

Compare: Tacx NEO 2T dump = CPS + CSC + FE-C, no FTMS (by design). This
KICKR = CPS + Wahoo custom, no FTMS, no CSC in the dump.

## Follow-ups

1. Finish “Read all” (or selective reads): Manufacturer, Model, Hardware,
   Battery, CP Feature hex (Firmware already known: **3.4.68**).
2. Confirm in Wahoo app / release notes for **3.4.68** whether FTMS is
   advertised; if yes, dump again after any required pairing/mode.
3. Optional: second dump while Zwift or the Wahoo app is *not* holding a
   second link, in case services are gated (unusual for FTMS).
