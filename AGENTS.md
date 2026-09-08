# Agent Instructions for RFIDDataLogger

This file guides AI coding agents (and contributors) working on this repository.

## Project Context
This is embedded C/C++ firmware (Arduino framework) for a solar/battery-powered field
RFID fish tag logger. It must survive 2+ weeks unattended on a single 1S LiFePO4 cell,
so **power consumption is the top priority in every code change**.

Before editing code, read [Specs/PROJECT_SPECS.md](Specs/PROJECT_SPECS.md) and
[Specs/EngineeringIssues.txt](Specs/EngineeringIssues.txt) for the power topology,
memory layout, and hardware wiring constraints.

## Repository Layout
- `Code/DataLogging.C` — main deployed firmware: RTC timestamping, dual-FRAM binary
  logging, deep-sleep wake-on-tag interrupt.
- `Code/HardwareWakeup.C` — standalone bring-up sketch used only to validate the
  wake-interrupt wiring in isolation; not linked with `DataLogging.C`.
- `Code/DataDownload.C` — standalone sketch flashed only during field data retrieval;
  dumps FRAM contents as CSV over USB serial.
- `Specs/PROJECT_SPECS.md` — architecture, BOM, memory map, firmware state machine.
- `Specs/EngineeringIssues.txt` — scanner/logger power and logic interconnect notes.

Each `.C` file is a **separate, independently-flashed Arduino sketch**. Do not merge
their `setup()`/`loop()` functions into one build — each targets a different phase of
the hardware bring-up/deployment/retrieval lifecycle.

## Hardware Constraints That Affect Code
- Artemis GPIOs are 3.3V only. The WL-134 scanner's TX line is 5V and is bridged
  through a level shifter (see PROJECT_SPECS.md Section 2a) before reaching `RX1` and
  the wake-up interrupt pin — never wire or code around this shifter.
- The WL-134 must be powered from a linear/LDO regulator, not a switching converter
  (switching noise degrades the 134.2 kHz antenna field). This is a hardware concern,
  but keep it in mind if adding any code-driven power control.
- FRAM boards are addressed at `0x50` and `0x51`; each stores 32,768 bytes; records
  are fixed at 12 bytes (4-byte little-endian Unix timestamp + 8-byte tag ID).
- The system pointer (current write address) lives at address `0` on FRAM board 1.

## Coding Conventions & Pitfalls
- **Avoid the Arduino `String` class** in code paths that run continuously during
  deployment (heap fragmentation over a 2-week run). Use fixed-size `char` buffers.
- **Never busy-poll** `Serial1.available()` in the main loop of deployed firmware —
  use the falling-edge interrupt + `am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP)`
  pattern so the MCU stays in deep sleep between tag reads.
- **ISRs must stay minimal**: only set a `volatile` flag; do no I2C, Serial, or delay
  calls inside an interrupt handler.
- **Debounce duplicate tag reads**: a fish lingering in the antenna field can trigger
  many repeat reads of the same tag; suppress repeats within a few seconds.
- Keep binary packing little-endian and consistent across `DataLogging.C` and
  `DataDownload.C` — they must agree on byte order or exported CSVs will be corrupt.
- When changing the record size or memory map, update both the firmware and
  `Specs/PROJECT_SPECS.md` together.

## Testing
There is no CI/build harness in this repo (Arduino IDE / arduino-cli target). When
changing code, verify manually:
- Code compiles for the SparkFun Apollo3 (Artemis) board target.
- Logic changes are consistent between `DataLogging.C` and `DataDownload.C` (shared
  binary format) if one is touched.
