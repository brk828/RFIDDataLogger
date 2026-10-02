/*
 * MKR Zero WL-134 bring-up prototype logger.
 * See Specs/MKRZero_Prototype_Wiring.md for wiring and
 * Specs/MKRZero_LoggerSketch.md for a walkthrough of this sketch and the
 * bugs it fixes relative to the original bench version.
 *
 * NOT the field firmware — see Code/DataLogging.C for the Artemis/FRAM design.
 */

#include <SPI.h>
#include <SD.h>
#include <RTCZero.h>
#include "wiring_private.h" // g_APinDescription, needed for the pull-up register poke below

const int SD_CS = SDCARD_SS_PIN;
const int WL134_RESET_PIN = 6;
const uint8_t WL134_RX_PIN = 13;

RTCZero rtc; // onboard SAMD21 RTC - keeps time only while powered, no battery backup

const uint32_t RESET_INTERVAL_MS = 1000; // re-trigger a lingering tag at most this often
const uint32_t FRAME_TIMEOUT_MS  = 200;  // abandon a frame if ETX doesn't arrive in time
const uint8_t  TAG_WINDOW_LEN    = 13;   // empirically-confirmed tag window size (see spec doc)

// pinMode() overwrites the whole PINCFG register and would clear PMUXEN,
// disconnecting this pin from the SERCOM/Serial1 RX function. Set only the
// pull-up bitfield instead, after Serial1.begin() has already set PMUXEN,
// so the diode level-shift (see wiring doc) has a 3.3V pull-up to work against.
void enableRxPullup(uint8_t pin) {
  EPortType port = g_APinDescription[pin].ulPort;
  uint32_t pinNum = g_APinDescription[pin].ulPin;
  PORT->Group[port].PINCFG[pinNum].bit.PULLEN = 1;
  PORT->Group[port].OUTSET.reg = (1ul << pinNum); // OUT=1 selects pull-up (not pull-down)
}

// Confirms PMUXEN (SERCOM routing) and PULLEN survived the register poke above.
// Needs only USB Serial - no WL-134 or diode wiring required.
void printPinCfg(uint8_t pin) {
  EPortType port = g_APinDescription[pin].ulPort;
  uint32_t pinNum = g_APinDescription[pin].ulPin;
  uint8_t cfg = PORT->Group[port].PINCFG[pinNum].reg;
  Serial.print("pin "); Serial.print(pin);
  Serial.print(" PINCFG=0x"); Serial.print(cfg, HEX);
  Serial.print(" PMUXEN="); Serial.print((cfg >> 0) & 1);
  Serial.print(" PULLEN="); Serial.println((cfg >> 2) & 1);
}

// Loopback self-test: jumper pin 14 (Serial1 TX) to pin 13 (Serial1 RX) in
// place of the WL-134/diode and confirm bytes sent are received back.
// Functionally proves the SERCOM RX path still works, no reader board needed.
void loopbackSelfTest() {
  while (Serial1.available()) Serial1.read(); // flush
  Serial1.write('X');
  uint32_t start = millis();
  while (!Serial1.available() && millis() - start < 50);
  bool ok = Serial1.available() && Serial1.read() == 'X';
  Serial.println(ok ? "Serial1 loopback OK" : "Serial1 loopback FAILED (no echo)");
}

bool readFrame(char *out, uint8_t maxLen) {
  uint8_t idx = 0;
  uint32_t start = millis();
  bool sawStx = false;

  while (millis() - start < FRAME_TIMEOUT_MS) {
    if (!Serial1.available()) continue;
    char c = Serial1.read();

    if (!sawStx) {
      if (c == 0x02) sawStx = true; // wait for STX before buffering
      continue;
    }

    if (idx < maxLen - 1) out[idx++] = c;

    if (c == 0x03) { // ETX: frame complete
      out[idx] = '\0';
      return true;
    }
  }

  out[idx] = '\0';
  return false; // timed out before ETX arrived
}

bool checksumOK(const char *frame, uint8_t len) {
  if (len < 4) return false;
  uint8_t chk = frame[len - 3];
  uint8_t inv = frame[len - 2];
  return (chk ^ inv) == 0xFF;
}

// Tag payload is ASCII text (not hex-encoded) LSB-character-first. The WL-134
// pads the payload to 26 chars, but only the first 13 hold this tag's data;
// reversing them character-by-character and splitting after 3 chars yields
// the "CCC.NNNNNNNNNN" format (e.g. 3D9.1C2D23F85D).
bool extractTagID(const char *frame, uint8_t frameLen, char *out) {
  // frame = payload starting after STX, ending with [CHK][INVCHK][ETX]
  int payloadLen = frameLen - 3;
  if (payloadLen < TAG_WINDOW_LEN) return false;

  // Build "CCC" + "." + remaining 10 chars from the reversed window
  char reversed[TAG_WINDOW_LEN + 1];
  for (uint8_t i = 0; i < TAG_WINDOW_LEN; i++) {
    reversed[i] = frame[TAG_WINDOW_LEN - 1 - i];
  }
  reversed[TAG_WINDOW_LEN] = '\0';

  memcpy(out, reversed, 3);
  out[3] = '.';
  memcpy(out + 4, reversed + 3, TAG_WINDOW_LEN - 3);
  out[4 + (TAG_WINDOW_LEN - 3)] = '\0';
  return true;
}

// Seeds the onboard RTC from the sketch's compile time. Only runs once at
// boot, so re-flashing (or power loss, since there's no RTC battery backup)
// is the only way the clock gets corrected.
void setRtcFromCompileTime() {
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4];
  int day, year, hour, minute, second;
  sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
  int month = (strstr(months, monStr) - months) / 3 + 1;
  rtc.setDate(day, month, year % 100);
  rtc.setTime(hour, minute, second);
}

void formatTimestamp(char *out) {
  sprintf(out, "%04d-%02d-%02d %02d:%02d:%02d",
          2000 + rtc.getYear(), rtc.getMonth(), rtc.getDay(),
          rtc.getHours(), rtc.getMinutes(), rtc.getSeconds());
}

void logCSV(const char *ts, const char *tag) {
  File f = SD.open("rfidlog.csv", FILE_WRITE);
  if (!f) {
    Serial.println("ERROR: SD.open(rfidlog.csv) failed - no card or write-protected?");
    return;
  }
  f.print(ts);
  f.print(",");
  f.println(tag);
  f.close();
  Serial.println("Logged to SD.");
}

void pulseReset() {
  digitalWrite(WL134_RESET_PIN, LOW);
  delay(40);
  digitalWrite(WL134_RESET_PIN, HIGH);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);
  enableRxPullup(WL134_RX_PIN); // must run after begin(); see comment above enableRxPullup()
  printPinCfg(WL134_RX_PIN);    // confirm PMUXEN+PULLEN both survived, no hardware needed
  // loopbackSelfTest(); // uncomment + jumper pin 14->13 to confirm RX actually works

  rtc.begin();
  setRtcFromCompileTime();

  pinMode(WL134_RESET_PIN, OUTPUT);
  digitalWrite(WL134_RESET_PIN, HIGH);

  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD Card init failed! Check card is FAT16/FAT32 and seated.");
  } else {
    Serial.println("SD Card ready.");
  }

  Serial.println("WL-134 PIT Tag Logger Ready.");
}

uint32_t lastResetMillis = 0;
bool readInProgress = false;

void loop() {
  // Never reset while a frame might be mid-transmission; only re-arm after
  // an idle period so a lingering tag gets a fresh read.
  if (!readInProgress && millis() - lastResetMillis >= RESET_INTERVAL_MS) {
    pulseReset();
    lastResetMillis = millis();
  }

  if (!Serial1.available()) return;

  readInProgress = true;
  char frame[64];
  bool complete = readFrame(frame, sizeof(frame));
  readInProgress = false;

  uint8_t frameLen = strlen(frame);
  if (!complete || frameLen < 4) {
    Serial.println("WARN: incomplete frame (timed out before ETX).");
    return;
  }

  if (!checksumOK(frame, frameLen)) {
    Serial.println("WARN: checksum failed, discarding frame.");
    return;
  }

  char tag[16];
  if (!extractTagID(frame, frameLen, tag)) {
    Serial.println("WARN: frame too short to extract tag.");
    return;
  }

  char ts[20];
  formatTimestamp(ts);
  Serial.print(ts);
  Serial.print(" TAG: ");
  Serial.println(tag);

  logCSV(ts, tag);
}
