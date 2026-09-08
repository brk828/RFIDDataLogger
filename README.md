# RFIDDataLogger

Ultra-low-power RFID PIT tag data logger for remote fish detection deployments (streams, bypass channels). Full design rationale lives in [Specs/PROJECT_SPECS.md](Specs/PROJECT_SPECS.md); known hardware integration gotchas are tracked in [Specs/EngineeringIssues.txt](Specs/EngineeringIssues.txt).

## Overview
- **Scanner:** WL-134 134.2 kHz RFID exciter/reader, driving a hand-matched antenna coil.
- **Logger:** SparkFun Thing Plus - Artemis (Ambiq Apollo3 Cortex-M4) with a Qwiic RV-1805 RTC and two Qwiic MB85RC256V FRAM boards (64 KB combined, ~5,460 records @ 12 bytes/record).
- **Power:** Isolated dual-battery design — a 1S LiFePO4 cell feeds the logger directly at 3.3V; a 2S LiFePO4 pack feeds the WL-134 through a 5V LDO (never a switching regulator, to avoid RF noise). Both packs share a common ground.
- **Logic Safety:** A dual-channel level shifter steps the WL-134's 5V TX line down to 3.3V, feeding both the Artemis hardware UART and a wake-up interrupt pin.
- **Target Runtime:** 2+ weeks unattended, waking from deep sleep only when a tag read begins.

## Repository Layout
```
Code/
  DataLogging.C     - Main logger firmware (RTC + FRAM logging, deep-sleep wake-on-tag)
  HardwareWakeup.C  - Standalone bring-up sketch for validating the wake-interrupt wiring
  DataDownload.C    - Standalone retrieval sketch: dumps logged records as CSV over USB
Specs/
  PROJECT_SPECS.md      - Full architecture, power topology, memory layout, firmware states
  EngineeringIssues.txt - Scanner/logger power & logic interconnect notes
```

## Getting Started
1. Wire the hardware per Section 2a of [PROJECT_SPECS.md](Specs/PROJECT_SPECS.md) (LDO regulator, level shifter, common ground).
2. Flash `Code/HardwareWakeup.C` first to confirm the wake-interrupt wiring is triggering correctly.
3. Flash `Code/DataLogging.C` as the deployed firmware.
4. When retrieving data in the field, flash `Code/DataDownload.C`, open a serial terminal at 115200 baud, and send `d` to dump CSV or `c` to clear the log.

Each `.C` file is an independent Arduino sketch (own `setup()`/`loop()`) — flash them one at a time rather than combining them into a single build.