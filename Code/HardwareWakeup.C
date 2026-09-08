/**
 * Wake-Up Interrupt Bring-Up Test
 * Target Hardware: SparkFun Thing Plus - Artemis (Ambiq Apollo3)
 * Purpose: Standalone hardware validation sketch. Flash this ALONE (not alongside
 *          DataLogging.C) to confirm the WL-134 -> level shifter -> WAKEUP_PIN wiring
 *          correctly wakes the Artemis from deep sleep before trusting the full
 *          logging firmware. The production wake+sleep logic lives in DataLogging.C.
 * Wiring: The WL-134's 5V TX line must be split through a dual-channel level shifter
 *         (see Specs/PROJECT_SPECS.md Section 2a) into two isolated 3.3V outputs:
 *         1. RX1 (to read the serial text)
 *         2. WAKEUP_PIN (to handle the wake-up interrupt)
 */

#include <Wire.h>

// --- Pin Allocations ---
const byte WAKEUP_PIN = 4; // Shifted (3.3V) copy of the WL-134 TX line

// Global flag to track when a fish triggers a wake-up event
volatile bool fishDetected = false; 

void setup() {
  Serial.begin(115200);
  
  // Initialize Hardware Serial 1 (RX/TX) for data tracking
  Serial1.begin(9600); 

  // Configure the interrupt pin with an internal pull-up resistor.
  // Idle state for Serial UART is HIGH (3.3V). When data starts transmitting, 
  // the start bit pulls the line LOW (0V). This creates a FALLING edge.
  pinMode(WAKEUP_PIN, INPUT_PULLUP);
  
  // Attach the interrupt function to trigger exactly when the line drops LOW
  attachInterrupt(digitalPinToInterrupt(WAKEUP_PIN), rfidWakeupISR, FALLING);

  Serial.println("Wake-up test armed. Dropping into low-power sleep...");
}

void loop() {
  // If no fish has swum past, send the Apollo3 chip into deep sleep
  if (!fishDetected) {
    // This Artemis SDK command halts the CPU clock, dropping current 
    // down to micro-amps. Peripherals like Serial1 remain armed.
    am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP); 
  }

  // --- THE SYSTEM IS NOW AWAKE ---
  // The moment the shifted RFID TX line drops LOW, the interrupt fires, 
  // wakes up the CPU, and code execution resumes right here.

  if (fishDetected) {
    // Give the hardware a tiny millisecond window to let the Serial buffer fill
    delay(10); 

    if (Serial1.available() > 0) {
      Serial.print("Wake confirmed. Raw bytes: ");
      while (Serial1.available() > 0) {
        Serial.write(Serial1.read());
      }
      Serial.println();
    }

    // Reset the tracking flag and prepare to drop back to deep sleep
    fishDetected = false;
    Serial.println("Wake cycle complete. Re-arming sleep mode...");
  }
}

/**
 * Interrupt Service Routine (ISR). 
 * This code must execute instantly. Do NOT put prints, delays, or I2C code inside here.
 */
void rfidWakeupISR() {
  fishDetected = true; 
}
