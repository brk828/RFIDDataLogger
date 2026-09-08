/**
 * Data Retrieval and CSV Export Routine
 * Target Hardware: SparkFun Thing Plus - Artemis + Dual FRAM Module Stack
 * Instructions: Open your Serial Monitor at 115200 baud, type 'd' and hit Enter.
 */

#include <Wire.h>
#include <SparkFun_MB85RC256V.h>

#define RECORD_SIZE         12
#define FRAM_BOARD_SIZE     32768
#define POINTER_ADDRESS     0

#define FRAM_ADDR_1         0x50
#define FRAM_ADDR_2         0x51

MB85RC256V fram1;
MB85RC256V fram2;

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Initialize both memory banks
  if (fram1.begin(FRAM_ADDR_1) == false || fram2.begin(FRAM_ADDR_2) == false) {
    Serial.println("Error connecting to FRAM hardware module stack.");
    while (1);
  }

  Serial.println("==================================================");
  Serial.println("      Fish PIT Tag Memory Retrieval Console      ");
  Serial.println("==================================================");
  Serial.println("Instructions: Send 'd' via terminal to dump CSV data.");
  Serial.println("Instructions: Send 'c' to CLEAR/RESET ALL records.");
  Serial.println("==================================================");
}

void loop() {
  if (Serial.available() > 0) {
    char command = Serial.read();
    
    if (command == 'd' || command == 'D') {
      dumpDataAsCSV();
    } 
    else if (command == 'c' || command == 'C') {
      clearMemoryLog();
    }
  }
}

/**
 * Reads binary records sequentially, decodes variables, and prints clean CSV formatting.
 */
void dumpDataAsCSV() {
  // Read where the active pointer currently sits to know where to stop
  uint32_t finalWriteAddress = getStoredWritePointer();

  Serial.println("\n--- START OF CSV DATA ---");
  Serial.println("Record_Index,Unix_Timestamp,PIT_Tag_ID"); // Print CSV Header Row

  uint32_t currentReadAddress = RECORD_SIZE; // Skip the address pointer block
  uint32_t recordCount = 0;

  while (currentReadAddress < finalWriteAddress) {
    uint8_t buffer[RECORD_SIZE];

    // Read the 12-byte payload from the correct physical board
    if (currentReadAddress < FRAM_BOARD_SIZE) {
      fram1.readBlock(currentReadAddress, buffer, RECORD_SIZE);
    } else {
      uint32_t board2Offset = currentReadAddress - FRAM_BOARD_SIZE;
      fram2.readBlock(board2Offset, buffer, RECORD_SIZE);
    }

    // --- Decode 4-Byte Timestamp (Little Endian) ---
    uint32_t timestamp = ((uint32_t)buffer[3] << 24) |
                          ((uint32_t)buffer[2] << 16) |
                          ((uint32_t)buffer[1] << 8)  |
                          buffer[0];

    // --- Decode 8-Byte Tag ID (Little Endian) ---
    uint64_t tagID = 0;
    for (int i = 0; i < 8; i++) {
      tagID |= ((uint64_t)buffer[4 + i] << (i * 8));
    }

    // --- Output CSV Line ---
    Serial.print(recordCount);
    Serial.print(",");
    Serial.print(timestamp);
    Serial.print(",");
    
    // Print the full 15-digit fish tag integer format natively
    printUint64(tagID); 
    Serial.println();

    currentReadAddress += RECORD_SIZE;
    recordCount++;
  }
  
  Serial.println("--- END OF CSV DATA ---");
  Serial.print("Total Records Exported: ");
  Serial.println(recordCount);
}

/**
 * Resets the master address index back to base, resetting the logger for a new deployment.
 */
void clearMemoryLog() {
  Serial.println("\nWARNING: This will wipe all current log limits. Confirm by typing 'Y': ");
  while (!Serial.available()); // Wait for verification character
  char confirm = Serial.read();

  if (confirm == 'Y' || confirm == 'y') {
    uint32_t resetAddress = RECORD_SIZE;
    uint8_t ptrBuffer[4];
    
    ptrBuffer[0] = (resetAddress & 0x000000FF);
    ptrBuffer[1] = (resetAddress & 0x0000FF00) >> 8;
    ptrBuffer[2] = (resetAddress & 0x00FF0000) >> 16;
    ptrBuffer[3] = (resetAddress & 0xFF000000) >> 24;
    
    fram1.writeBlock(POINTER_ADDRESS, ptrBuffer, 4);
    Serial.println("Memory cleared successfully. Ready for clean deployment.");
  } else {
    Serial.println("Clear command aborted.");
  }
}

/**
 * Helper function to retrieve the absolute boundary marker from Board 1.
 */
uint32_t getStoredWritePointer() {
  uint8_t ptrBuffer[4];
  fram1.readBlock(POINTER_ADDRESS, ptrBuffer, 4);
  
  return ((uint32_t)ptrBuffer[3] << 24) | 
         ((uint32_t)ptrBuffer[2] << 16) | 
         ((uint32_t)ptrBuffer[1] << 8)  | 
         ptrBuffer[0];
}

/**
 * Built-in printer helper to prevent Arduino IDE from dropping 64-bit integer numbers.
 */
void printUint64(uint64_t num) {
  char rev[21];
  int i = 0;
  if (num == 0) {
    Serial.print('0');
    return;
  }
  while (num > 0) {
    rev[i++] = (num % 10) + '0';
    num /= 10;
  }
  while (i > 0) {
    Serial.print(rev[--i]);
  }
}
