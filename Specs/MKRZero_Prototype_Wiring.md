# MKR Zero Bring-Up Prototype — Wiring Specification

## Purpose
This is a **bench bring-up prototype**, separate from the Artemis-based field
deployment design in [PROJECT_SPECS.md](PROJECT_SPECS.md). It uses an Arduino
MKR Zero (not the Artemis Thing Plus) to validate WL-134 tag reads, reset
behavior, and tag-code extraction before porting the logic to the final
low-power firmware. It is powered over USB on the bench, not by the field
battery packs.

## Bill of Materials
- Arduino MKR Zero
- Priority1Design WL-134 134.2 kHz RFID reader/exciter module
- MKR Zero onboard microSD slot (used directly — no Qwiic FRAM at this stage)
- 9V battery (or bench supply) dedicated to the WL-134 exciter coil
- microSD card, **FAT16 or FAT32** (not exFAT)

## Pin Map

| WL-134 Pin | Signal | MKR Zero Pin | Notes |
|---|---|---|---|
| TX | Serial data out | Pin 13 (`Serial1` RX) | WL-134 TX is read directly; see Section "5V vs 3.3V" below |
| RST | Reset input | Pin 6 | Driven LOW to pulse reset, held HIGH otherwise |
| GND | Ground | MKR Zero GND | Must be common with battery negative |
| +9V | Power | 9V battery + | Dedicated supply, isolated from MKR Zero logic |
| — | Battery − | MKR Zero GND | Ties scanner ground to logic ground |

MKR Zero itself is powered via USB from the host computer during bench testing.

## 5V → 3.3V Level Shift: Single-Diode Clamp
The MKR Zero's GPIOs (including pin 13 / `Serial1` RX) are **3.3V-tolerant
only**, but the WL-134 `TX` line is 5V logic. Rather than a full bidirectional
level shifter (used in the Artemis field design, see
[PROJECT_SPECS.md §2a](PROJECT_SPECS.md#2a-scanner-board--power-logic-interconnect)),
this prototype uses a single-diode clamp, since only one direction (WL-134 →
MKR Zero) needs shifting here:

1. Enable the MKR Zero's internal pull-up on pin 13 in code
   (`pinMode(13, INPUT_PULLUP)`), which weakly pulls the pin to 3.3V.
2. Diode **cathode** → WL-134 `TX`.
3. Diode **anode** → MKR Zero pin 13.

Use a small-signal switching diode (e.g. 1N4148), not a slower power
rectifier.

**Why this is safe:** when `TX` is HIGH (5V), the cathode sits above the
pulled-up anode, so the diode is reverse-biased (only ~1.7V reverse, well
under any silicon diode's rating) and no current flows — pin 13 is held at
3.3V by the pull-up alone and never sees 5V directly. When `TX` is LOW, the
diode forward-conducts and clamps pin 13 to roughly one diode drop (~0.6V)
above ground, which reads as a valid LOW. The WL-134's push-pull output
easily sinks the microamp-scale current from the internal pull-up.

**Gotcha — plain `pinMode()` is not safe here, in either order:**
`Serial1.begin()` configures pin 13 for the SERCOM peripheral by setting its
`PMUXEN` bit. The SAMD core's `pinMode()` does a full overwrite of that pin's
`PINCFG` register rather than a read-modify-write, so calling `pinMode(13,
INPUT_PULLUP)` — before *or* after `Serial1.begin()` — clears `PMUXEN` and
silently disconnects pin 13 from `Serial1` RX entirely, not just the
pull-up. `Code/MKRZeroPrototype.C` instead pokes only the `PULLEN`/`OUT`
bitfields directly on the PORT registers *after* `Serial1.begin()`, leaving
`PMUXEN` untouched. **Confirm with a multimeter that pin 13 idles at ~3.3V**
after `setup()` runs, and that `Serial1` still receives data, before trusting
this on the bench.

This clamp only shifts the one `TX` → RX direction used here. If a second,
interrupt-driven wake pin (like `WAKEUP_PIN` in the Artemis design) is ever
added to this bench prototype, it needs its own diode + pulled-up GPIO pair
tied to the same WL-134 `TX` line — the two can share the same `TX` source
since a diode's reverse-bias isolates each receiving pin from the others.

## Serial Configuration
- `Serial` (USB) — 115200 baud, used only for debug/console output.
- `Serial1` (hardware UART, pin 13 RX / pin 14 TX) — 9600 baud, connected to
  the WL-134 TX line. Only RX is used; the MKR Zero never transmits to the
  WL-134 over this line.

## WL-134 Frame Format (as observed on this bench unit)
```
0x02 (STX) | ASCII payload (26 characters) | checksum byte | inverted checksum byte | 0x03 (ETX)
```
- The ASCII payload is **not** hex-encoded binary — the WL-134 transmits the
  tag code directly as ASCII text, LSB-character-first.
- Checksum validity rule observed: `checksum_byte XOR inverted_checksum_byte == 0xFF`.
- See [MKRZero_LoggerSketch.md](MKRZero_LoggerSketch.md) for how the tag code
  is extracted from the payload.

## Reset Behavior
Pulsing the WL-134 `RST` pin LOW briefly (~40 ms) then back HIGH forces the
module to re-arm its reader state, which causes it to re-transmit a tag that
is still sitting in the antenna field (fish not swimming away). This is used
instead of relying on the WL-134 to repeat reads on its own.
