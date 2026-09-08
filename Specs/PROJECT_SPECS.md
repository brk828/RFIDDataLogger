# Project Specification: Low-Power Fish PIT Tag Data Logger

## 1. Project Overview
This project defines an ultra-low-power, solderless serial data logger designed to capture 134.2 kHz RFID fish PIT tag records in remote field environments (e.g., streams, bypass channels). It replaces legacy microcontroller architectures (like the PIC18F25K20) and high-power microSD layouts with a modern ARM Cortex-M4 architecture and non-volatile Ferroelectric RAM (FRAM).

### Core Goals
- **Runtime:** Minimum 2 weeks (14 days) continuous field deployment.
- **Power Optimization:** Eradicate high-current storage write spikes (50-100mA microSD spikes) to protect a strict power budget.
- **Form Factor:** Purely modular, off-the-shelf, solderless "Qwiic" I2C hardware chaining to eliminate custom PCB design requirements.
- **Data Target:** Safely log a buffer of up to 5,000 distinct fish records.

---

## 2. System Architecture & Power Topology

### Power Isolation Strategy
To prevent electrical noise, voltage sag, and field failure, the system implements **isolated dual-battery power tracks**:
1. **Scanner Power (2S Pack):** A dedicated 2S LiFePO4 battery pack (~6.4V to 7.2V) strictly running the high-draw RFID antenna/exciter board (~80 mA average).
2. **Logger Power (1S Pack):** A isolated **1S 26650 LiFePO4 cell (3.2V Nominal, ~3,000 to 3,500 mAh)** feeding directly into the logger stack's `3V3` rail. This provides a 30%+ safety margin over the calculated 2-week worst-case draw (~2,184 mAh).

### Hardware Bill of Materials (BOM)
All core logging modules interface via the **SparkFun Qwiic (I2C)** ecosystem:
* **Processor Board:** SparkFun Thing Plus - Artemis (Ambiq Apollo3 Cortex-M4)
* **Real-Time Clock:** SparkFun Qwiic Real-Time Clock Module (RV-1805)
* **Storage Module 1:** SparkFun Qwiic FRAM Breakout - 256Kbit / 32KB (MB85RC256V) [Default Address: `0x50`]
* **Storage Module 2:** SparkFun Qwiic FRAM Breakout - 256Kbit / 32KB (MB85RC256V) [Bridged Address Jumper A0: `0x51`]
* **RFID Scanner Board:** SparkFun/Priority1Design **WL-134** 134.2 kHz exciter/reader module, matched to a custom antenna coil (already tuned/built by the hardware team).
* **Voltage Regulation:** Dedicated 5V Linear/LDO regulator (e.g., low-noise LDO such as an MCP1700/AP2114 class part) stepping the 2S LiFePO4 pack down to the WL-134's 5V supply rail.
* **Logic Isolation:** Dual-channel bidirectional logic level shifter (3.3V LV / 5V HV) to safely bridge the WL-134's 5V TX signal down to the Artemis' 3.3V-only GPIO.
* **Cabling:** Assorted SparkFun Qwiic flexible ribbon cables.

---

## 2a. Scanner Board & Power/Logic Interconnect

### WL-134 Scanner Board Pinout
| WL-134 Pin | Function | Connects To |
|---|---|---|
| Pin 1 | +5V (or +9V) Power Input | 5V LDO regulator output |
| Pin 3 | TX (Serial Data Out, 5V logic) | Level shifter HV-side inputs (spliced to both channels) |
| Pin 4 | GND | 5V LDO regulator GND / shared system ground |

### Power Distribution Solution
The WL-134 datasheet warns that switching (buck) regulators inject electromagnetic noise into the 134.2 kHz antenna field and measurably degrade read range. To protect performance, power for the scanner **must** come from a **linear/LDO regulator**, never a switching converter:

1. **2S LiFePO4 pack → 5V Linear/LDO regulator → WL-134 Pin 1 (+5V) / Pin 4 (GND).** This isolates the noisy high-current antenna exciter from the logger's logic supply while keeping the RF environment clean.
2. **1S LiFePO4 pack → Artemis `3V3`/`GND` directly.** This is the isolated, low-noise rail dedicated to the logger stack (RTC, FRAM, MCU).
3. **Common Ground Requirement:** Even though the two battery tracks are electrically isolated, the WL-134 GND and the Artemis GND **must** be tied together at one point. Without a shared ground reference, the serial data and interrupt signals crossing between the two power domains will not register reliably.

### Logic-Level Interconnect Solution
The WL-134 transmits serial data at 5V logic, but the Artemis Cortex-M4 GPIOs are strictly 3.3V-tolerant. Driving them directly with the WL-134's 5V TX line risks permanent pin damage, so a dual-channel level shifter is required:

| From Pin | To Level Shifter Pin | Shifter Output Pin | To Artemis Pin | Purpose |
|---|---|---|---|---|
| Artemis `3V3` | `LV` | — | — | Low-voltage side supply for shifter |
| WL-134 5V Rail | `HV` | — | — | High-voltage side supply for shifter |
| Shared Ground | `GND` | `GND` | `GND` | Common ground reference for both sides |
| WL-134 Pin 3 (TX) | `H1` | `L1` | `RX1` (Serial Input) | Decoded tag data stream |
| WL-134 Pin 3 (TX) | `H2` | `L2` | `WAKEUP_PIN` (Pin 4, interrupt) | Wake-up edge detection |

Splicing the single WL-134 TX line into two high-side shifter channels (`H1`/`H2`) produces two independent, safe 3.3V outputs (`L1`/`L2`): one feeds the Artemis hardware UART (`RX1`) for decoding the tag string, and the other feeds a GPIO interrupt pin used solely to wake the Apollo3 core from deep sleep the instant a tag transmission begins (falling-edge start bit).

---

## 3. Data Layout & Memory Architecture
To optimize the combined 64 KB storage layout across the two physical FRAM boards, data structures must avoid verbose ASCII text representations and utilize compact binary packing.

### Binary Payload Structure (12 Bytes Per Fish)
Each fish read consumes exactly **12 bytes**:
1. **Timestamp (4 Bytes):** Standard 32-bit unsigned integer tracking Unix Epoch time.
2. **Tag ID (8 Bytes):** 64-bit unsigned integer (`uint64_t`) packing the 15-digit ISO fish identification number.

### Memory Map (65,536 Total Bytes Available)
* **FRAM Board 1 (`0x50`):** Addresses `0x0000` to `0x7FFF` (Stores records 0 to 2,730).
* **FRAM Board 2 (`0x51`):** Addresses `0x0000` to `0x7FFF` (Stores records 2,731 to 5,461).

---

## 4. Software Design Requirements (Agent Instructions)
When designing the firmware in the Arduino IDE environment, the following operational logic must be executed:

### State 1: Initialization (`setup`)
- Initialize the I2C bus (`Wire`).
- Verify presence of the RV-1805 RTC and both FRAM instances (`0x50` and `0x51`).
- Read a persistent "current record pointer" from a designated system byte in FRAM to determine where to resume writing if the system reboots.
- Configure Hardware `Serial1` (RX/TX pins) to the exact baud rate of the external RFID scanner board.

### State 2: Low-Power Loop (`loop`)
- Keep the Artemis core in deep sleep (`am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP)`) whenever no tag is being processed.
- Use a Pin-Change/Falling-Edge Interrupt on the shifted (3.3V) copy of the RFID scanner's TX line (see Section 2a) to instantly wake the processor the moment a tag transmission begins. Do not busy-poll `Serial1.available()`, as this defeats the low-power budget.
- Debounce repeat detections: ignore re-reads of the same Tag ID within a short window (e.g., 3-5 seconds) so a lingering fish doesn't generate dozens of duplicate records.

### State 3: Data Ingestion & Conversion
- Read incoming raw serial ASCII text string from the scanner (e.g., `"989000012345678"`).
- Convert the string payload into a `uint64_t` integer.
- Fetch current 4-byte timestamp from the RV-1805 RTC.
- Pack fields into a 12-byte block.

### State 4: Sequential Paging & Boundary Conditions
- Check current record address pointer.
- If address `< 32,768`, write directly to `0x50` (Board 1).
- If address `≥ 32,768`, subtract 32,768 from the index and write to `0x51` (Board 2).
- Update the persistent pointer memory address.
- Immediately return system to low-power state.

---

## 5. Next Steps for Implementation
1. **Hardware Preparation:** Physically bridge the **A0** address solder pad on the back of the second Qwiic FRAM module to set it to `0x51`.
2. **Framework Code Generation:** Develop the firmware script leveraging `<Wire.h>`, SparkFun's RV-1805 library, and an I2C FRAM writing routine.
3. **Data Retrieval Interface:** Build a secondary terminal-dump routine where plugging the logger into a PC via USB-C outputs the binary data converted back into human-readable CSV format.
