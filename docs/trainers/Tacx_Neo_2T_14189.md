# Tacx NEO 2T — nRF Connect dump

- **Device name (file):** Tacx Neo 2T 14189  
- **Captured:** 2026-11-14 (~09:26 local)  
- **Tool:** nRF Connect (log export)  
- **Raw log:** `Tacx_Neo_2T_14189_nrf_connect.txt`  
- **Scope of this dump:** service/characteristic discovery after connect.  
  **Not captured:** characteristic value reads (CSC/CP Feature bits, Device
  Information strings, FE-C payloads). Re-read those in a follow-up if needed.

## Services discovered

| Service | UUID / name | Role for Homewind |
|---|---|---|
| Cycling Speed and Cadence | `0x1816` | Cadence (and speed) — already relaid today |
| Cycling Power | `0x1818` | Power — already relaid today as PWR |
| Tacx FE-C over BLE | `6E40FEC1-B5A3-F393-E0A9-E50E24DCCA9E` | Proprietary control/data (AGENT_FEATURES §3) |
| Secure DFU | Nordic DFU | Firmware update — ignore for proxy |
| Device Information | `0x180A` | Manufacturer / model / FW strings — not read yet |
| Proprietary | `CF285001-00DB-15A1-0E3F-FFA5794C3719` | Unknown Tacx/Garmin |
| Proprietary | `FE03A000-17D0-470A-8798-4AD3E1C1F35B` | Unknown |
| Proprietary | `FC82` (16-bit) | Unknown |
| Proprietary | `FA1C030E-52D1-2062-0777-BC61F0B8E27B` | Unknown |

## Standard characteristics

**CSC `0x1816`**
- CSC Measurement (notify, CCCD)
- CSC Feature (read)
- Sensor Location (read)
- SC Control Point (indicate, CCCD)

**Cycling Power `0x1818`**
- Cycling Power Measurement (notify, CCCD)
- Cycling Power Feature (read)
- Sensor Location (read)
- Cycling Power Vector (notify, CCCD)
- Cycling Power Control Point (indicate, CCCD)

**Device Information `0x180A`**
- Manufacturer Name, Model Number, Serial Number, Hardware Revision,
  Firmware Revision (read — values not in this log)

## Tacx FE-C over BLE (`6E40FEC1-…`)

| Characteristic | Likely role |
|---|---|
| `6E40FEC2-B5A3-F393-E0A9-E50E24DCCA9E` | TX / notify from trainer (CCCD present) |
| `6E40FEC3-B5A3-F393-E0A9-E50E24DCCA9E` | RX / write to trainer (no CCCD) |

Matches the FE-C-over-BLE shape in Homewind `docs/AGENT_FEATURES.md` §3
(RX/TX on service `6E40FEC1-…`). Full page map needs Garmin “Request Access”
or a captured workout.

## Finding for the FTMS proxy design (§2 / §3)

**This NEO 2T does not advertise Fitness Machine / FTMS (`0x1826`).**

Confirmed (maintainer, 2026-11-14): the **Tacx NEO series has no native
Bluetooth FTMS**. Garmin/Tacx uses proprietary BLE and ANT+/FE-C instead.
Garmin/Tacx state that FTMS is not suited to pass the virtual flywheel mass
and related algorithms the NEO needs; other Tacx lines (e.g. **Flux**) do
support FTMS. So absence of `0x1826` here is by design, not a missing app
toggle.

Control for ERG / simulation on a NEO is therefore **FE-C over BLE** (§3:
service `6E40FEC1-…`), not FTMS Control Point forwarding. Read-only power +
cadence for Zwift via Homewind already has a path: CPS + CSC. Controllable
proxy for NEO = FE-C ↔ FTMS (or FE-C ↔ app). Controllable proxy for Flux and
other FTMS trainers = §2.

## Follow-ups (no code yet)

1. Read Device Information → record firmware / hardware revision in this file.
2. Read CSC Feature and Cycling Power Feature → hex + decoded bits.
3. Optional: nRF dump of a **Flux** (or other FTMS Tacx) for §2 Feature bits.
4. Architecture decision: FTMS-first (§2) for the broad market, FE-C (§3) for
   NEO/older Tacx, or both with a shared Zwift-facing FTMS server.
