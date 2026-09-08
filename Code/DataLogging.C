/**
 * Low-Power Fish PIT Tag Data Logger Firmware Skeleton
 * Target Hardware: SparkFun Thing Plus - Artemis
 * Peripherals: RV-1805 RTC, 2x MB85RC256V Qwiic FRAM (0x50 and 0x51)
 *
 * RFID Interconnect: the WL-134's 5V TX line must be split through a dual-channel
 * level shifter before reaching this board (see Specs/PROJECT_SPECS.md Section 2a).
 * One shifted 3.3V output feeds RX1, the other feeds WAKEUP_PIN below.
 */

#include <Wire.h>
#include <SparkFun_RV1805_RTC_Arduino_Library.h>
#include <SparkFun_MB85RC256V.h> // Ensure you use the I2C version

// --- Hardware & Configuration Constants ---
#define RFID_BAUD_RATE      9600      // WL-134 default UART baud rate
#define RECORD_SIZE         12        // 4 bytes timestamp + 8 bytes Tag ID
#define FRAM_BOARD_SIZE     32768     // 32 KB per board
#define POINTER_ADDRESS     0         // Where we store our current write index in FRAM
#define TAG_STRING_MAX      24        // Max chars expected for an ASCII tag read (15 digits + margin)
#define DEBOUNCE_WINDOW_MS  4000      // Ignore repeat reads of the same tag within this window

// --- I2C Addresses ---
#define FRAM_ADDR_1         0x50      // Default address
#define FRAM_ADDR_2         0x51      // A0 jumper bridged

// --- Pin Allocations ---
const byte WAKEUP_PIN = 4; // Shifted (3.3V) copy of the WL-134 TX line, interrupt-only

// --- Global Object Instances ---
RV1805 rtc;
MB85RC256V fram1;
MB85RC256V fram2;

// --- Global Runtime Variables ---
uint32_t currentWriteAddress = RECORD_SIZE; // Start after the system pointer storage
volatile bool fishDetected = false;         // Set by the wake-up ISR, cleared after handling
uint64_t lastLoggedTagID = 0;               // For debounce comparison
uint32_t lastLoggedMillis = 0;

void setup() {
  // Initialize USB Serial for data download/debugging
  Serial.begin(115200);
  
  // Initialize Hardware Serial 1 (RX/TX pins) connected to RFID reader
  Serial1.begin(RFID_BAUD_RATE);

  // Initialize Qwiic I2C Bus
  Wire.begin();

  // 1. Initialize RTC
  if (rtc.begin() == false) {
    Serial.println("Error: RV-1805 RTC not detected.");
    while (1);
  }
  rtc.set24Hour();

  // 2. Initialize FRAM Boards
  if (fram1.begin(FRAM_ADDR_1) == false) {
    Serial.println("Error: FRAM Board 1 (0x50) not detected.");
    while (1);
  }
  if (fram2.begin(FRAM_ADDR_2) == false) {
    Serial.println("Error: FRAM Board 2 (0x51) not detected.");
    while (1);
  }

  // 3. Recover System Pointer (Where did we leave off before a power cycle?)
  recoverWritePointer();

  // 4. Arm the wake-up interrupt on the shifted WL-134 TX line.
  // Idle UART is HIGH; the start bit of a new tag transmission pulls it LOW.
  pinMode(WAKEUP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WAKEUP_PIN), rfidWakeupISR, FALLING);

  Serial.println("System armed. Dropping into low-power sleep between tag reads...");
}

void loop() {
  // Stay in deep sleep until the wake-up ISR flags an incoming tag transmission.
  // This keeps average current low across the full 2-week deployment.
  if (!fishDetected) {
    am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP);
    return;
  }

  // --- THE SYSTEM IS NOW AWAKE ---
  // Give the UART a short window to finish shifting in the tag string.
  delay(10);

  if (Serial1.available() > 0) {
    char rawTagBuf[TAG_STRING_MAX + 1];
    size_t len = readTagLine(rawTagBuf, sizeof(rawTagBuf));

    if (len > 0) {
      processAndLogTag(rawTagBuf);
    }
  }

  fishDetected = false;
}

/**
 * Interrupt Service Routine (ISR). Must execute instantly: no prints, delays, or I2C.
 */
void rfidWakeupISR() {
  fishDetected = true;
}

/**
 * Reads a newline-terminated ASCII tag string from Serial1 into a fixed buffer,
 * trimming trailing CR/LF. Avoids the Arduino String class to prevent heap
 * fragmentation over a multi-week deployment.
 */
size_t readTagLine(char *buf, size_t bufSize) {
  size_t idx = 0;
  uint32_t startMillis = millis();

  while (idx < bufSize - 1) {
    if (Serial1.available() > 0) {
      char c = Serial1.read();
      if (c == '\n' || c == '\r') {
        if (idx > 0) break; // End of a non-empty line
        continue;           // Skip leading CR/LF
      }
      buf[idx++] = c;
    } else if (millis() - startMillis > 100) {
      break; // Timeout: avoid blocking forever on a partial/garbled read
    }
  }

  buf[idx] = '\0';
  return idx;
}

/**
 * Packs timestamp and Tag ID into 12 binary bytes and manages multi-chip boundaries.
 */
void processAndLogTag(const char *tagStr) {
  // Convert 15-digit ASCII string into a compressed 64-bit integer.
  // Note: strtoull handles values larger than standard 32-bit longs safely.
  uint64_t tagID = strtoull(tagStr, NULL, 10);
  if (tagID == 0) {
    return; // Empty/garbled read, nothing valid to log
  }

  // Debounce: ignore repeat reads of the same tag within the debounce window
  // (a fish lingering in the antenna field re-triggers reads rapidly).
  uint32_t nowMillis = millis();
  if (tagID == lastLoggedTagID && (nowMillis - lastLoggedMillis) < DEBOUNCE_WINDOW_MS) {
    return;
  }

  // Fetch exact current time from RTC as a 32-bit Unix Timestamp
  rtc.updateTime();
  uint32_t timestamp = rtc.getEpoch();

  // Check for out-of-memory boundary condition (~5,460 tags max)
  if (currentWriteAddress + RECORD_SIZE > (FRAM_BOARD_SIZE * 2)) {
    Serial.println("CRITICAL ERROR: Memory full. Cannot log fish tag.");
    return; 
  }

  // Determine target board and write the 12-byte payload
  if (currentWriteAddress < FRAM_BOARD_SIZE) {
    // Payload fits completely on Board 1
    writeBinaryToFRAM(fram1, currentWriteAddress, timestamp, tagID);
  } 
  else {
    // Payload belongs on Board 2 (Offset address to stay within 0 - 32767 range)
    uint32_t board2Offset = currentWriteAddress - FRAM_BOARD_SIZE;
    writeBinaryToFRAM(fram2, board2Offset, timestamp, tagID);
  }

  // Increment pointer and save it persistently to memory address 0
  currentWriteAddress += RECORD_SIZE;
  saveWritePointer();

  lastLoggedTagID = tagID;
  lastLoggedMillis = nowMillis;

  // Debug statement (Can be commented out to save a micro-fraction of power)
  Serial.print("Logged Tag: "); Serial.print(tagStr);
  Serial.print(" at Address: "); Serial.println(currentWriteAddress - RECORD_SIZE);
}

/**
 * Handles raw byte breakdown and transfers to specific I2C FRAM memory slots.
 */
void writeBinaryToFRAM(MB85RC256V &framDevice, uint32_t destAddress, uint32_t timeData, uint64_t tagData) {
  uint8_t buffer[RECORD_SIZE];

  // Break 4-byte timestamp into buffer (Little Endian format)
  buffer[0] = (timeData & 0x000000FF);
  buffer[1] = (timeData & 0x0000FF00) >> 8;
  buffer[2] = (timeData & 0x00FF0000) >> 16;
  buffer[3] = (timeData & 0xFF000000) >> 24;

  // Break 8-byte Tag ID into buffer 
  for (int i = 0; i < 8; i++) {
    buffer[4 + i] = (tagData >> (i * 8)) & 0xFF;
  }

  // Write sequential block to the designated FRAM chip
  framDevice.writeBlock(destAddress, buffer, RECORD_SIZE);
}

/**
 * Reads persistent address 0 on Board 1 to track system progress across battery changes.
 */
void recoverWritePointer() {
  uint8_t ptrBuffer[4];
  fram1.readBlock(POINTER_ADDRESS, ptrBuffer, 4);
  
  // Reconstruct 32-bit pointer from bytes
  uint32_t recoveredValue = ((uint32_t)ptrBuffer[3] << 24) | 
                            ((uint32_t)ptrBuffer[2] << 16) | 
                            ((uint32_t)ptrBuffer[1] << 8)  | 
                            ptrBuffer[0];

  // If the chip is fresh/blank, format it to start after address pointer block
  if (recoveredValue == 0xFFFFFFFF || recoveredValue < RECORD_SIZE) {
    currentWriteAddress = RECORD_SIZE;
    saveWritePointer();
  } else {
    currentWriteAddress = recoveredValue;
  }
}

/**
 * Commits the current active index pointer safely to Board 1's header bytes.
 */
void saveWritePointer() {
  uint8_t ptrBuffer[4];
  ptrBuffer[0] = (currentWriteAddress & 0x000000FF);
  ptrBuffer[1] = (currentWriteAddress & 0x0000FF00) >> 8;
  ptrBuffer[2] = (currentWriteAddress & 0x00FF0000) >> 16;
  ptrBuffer[3] = (currentWriteAddress & 0xFF000000) >> 24;
  
  fram1.writeBlock(POINTER_ADDRESS, ptrBuffer, 4);
}
