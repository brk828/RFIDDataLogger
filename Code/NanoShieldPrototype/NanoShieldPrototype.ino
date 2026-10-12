/*
 * Arduino Nano 3 + data logging shield WL-134 bring-up prototype logger.
 * Ported from Code/MKRZeroPrototype/ (see Specs/MKRZero_LoggerSketch.md for
 * the frame-format walkthrough; framing logic is unchanged here).
 *
 * Differences from the MKR Zero version:
 *  - WL-134 is on SoftwareSerial (the Nano's only hardware UART is the
 *    USB/Serial connection). Reader TX is 5V TTL and drives the Nano pin
 *    directly - no diode level-shift or pull-up register poke needed.
 *  - RTC comes from the logging shield, battery-backed, so time persists
 *    across power loss. Compile-time seeding runs only when the RTC reports
 *    it lost power (dead/missing coin cell or first ever run).
 *  - SD CS is pin 10 (standard logging-shield wiring).
 *
 * NOT the field firmware — see Code/DataLogging.C for the Artemis/FRAM design.
 */

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <SoftwareSerial.h>

const int SD_CS = 10;          // standard data logging shield SD CS
const int WL134_RESET_PIN = 6; // WL-134 reset line
const uint8_t WL134_RX_PIN = 8; // Nano digital pin wired to reader TX (5V TTL, direct)
const uint8_t WL134_TX_PIN = 7; // Nano pin wired to reader RX (reader is 5V-tolerant input)

// SoftwareSerial at 9600 8N1 matches the WL-134 default UART settings.
SoftwareSerial readerSerial(WL134_RX_PIN, WL134_TX_PIN);

const uint32_t RESET_INTERVAL_MS = 1000; // re-trigger a lingering tag at most this often
const uint32_t FRAME_TIMEOUT_MS  = 200;  // abandon a frame if ETX doesn't arrive in time
const uint8_t  TAG_WINDOW_LEN    = 13;   // empirically-confirmed tag window size (see spec doc)

RTC_DS1307 rtc_ds1307;
RTC_DS3231 rtc_ds3231;
RTC_PCF8523 rtc_pcf8523;

// Which RTC chip the shield actually has; auto-detected in setup(). lostPower()
// and adjust() are not virtual across the RTClib classes, so chip-specific
// calls are dispatched here in detectRtc()/setRtcFromCompileTime() instead of
// through a base-class pointer.
uint8_t rtcChip = 0; // 0=none, 1=DS3231, 2=DS1307, 3=PCF8523
bool rtcLostPower = false;
const char *rtcName = "none";

// Loopback self-test: jumper WL134_TX_PIN to WL134_RX_PIN in place of the
// WL-134 and confirm bytes sent are received back.
void loopbackSelfTest() {
  while (readerSerial.available()) readerSerial.read(); // flush
  readerSerial.write('X');
  uint32_t start = millis();
  while (!readerSerial.available() && millis() - start < 50);
  bool ok = readerSerial.available() && readerSerial.read() == 'X';
  Serial.println(ok ? "SoftwareSerial loopback OK" : "SoftwareSerial loopback FAILED (no echo)");
}

bool readFrame(char *out, uint8_t maxLen) {
  uint8_t idx = 0;
  uint32_t start = millis();
  bool sawStx = false;

  while (millis() - start < FRAME_TIMEOUT_MS) {
    if (!readerSerial.available()) continue;
    char c = (char)readerSerial.read();

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
  int payloadLen = frameLen - 3;
  if (payloadLen < TAG_WINDOW_LEN) return false;

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

// Seeds the RTC from the sketch's compile time. Only called when the RTC
// reports lostPower() (first run, or dead/missing backup coin cell).
void setRtcFromCompileTime() {
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4];
  int day, year, hour, minute, second;
  sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
  int month = (strstr(months, monStr) - months) / 3 + 1;
  DateTime seed(year, month, day, hour, minute, second);
  switch (rtcChip) {
    case 1: rtc_ds3231.adjust(seed); break;
    case 2: rtc_ds1307.adjust(seed); break;
    case 3: rtc_pcf8523.adjust(seed); break;
  }
}

void formatTimestamp(char *out) {
  DateTime now;
  switch (rtcChip) {
    case 1: now = rtc_ds3231.now(); break;
    case 2: now = rtc_ds1307.now(); break;
    case 3: now = rtc_pcf8523.now(); break;
    default: strcpy(out, "0000-00-00 00:00:00"); return;
  }
  sprintf(out, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
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

// Detects which RTC chip is populated on the shield. begin() returns false
// for chips that aren't present on the I2C bus, so exactly one should match.
void detectRtc() {
  if (rtc_ds3231.begin()) {
    rtcChip = 1;
    rtcName = "DS3231";
    rtcLostPower = rtc_ds3231.lostPower();
  } else if (rtc_ds1307.begin()) {
    rtcChip = 2;
    rtcName = "DS1307";
    rtcLostPower = rtc_ds1307.lostPower();
  } else if (rtc_pcf8523.begin()) {
    rtcChip = 3;
    rtcName = "PCF8523";
    rtcLostPower = rtc_pcf8523.lostPower();
  }
}

void setup() {
  Serial.begin(115200);
  readerSerial.begin(9600);
  // loopbackSelfTest(); // uncomment + jumper WL134_TX_PIN->WL134_RX_PIN to confirm RX works

  pinMode(WL134_RESET_PIN, OUTPUT);
  digitalWrite(WL134_RESET_PIN, HIGH);

  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD Card init failed! Check card is FAT16/FAT32 and seated.");
  } else {
    Serial.println("SD Card ready.");
  }

  detectRtc();
  if (rtcChip == 0) {
    Serial.println("ERROR: no RTC found on shield (check I2C wiring/addresses).");
  } else {
    Serial.print("RTC detected: ");
    Serial.println(rtcName);
    // Battery-backed RTC: only set the clock if it lost power since last set.
    if (rtcLostPower) {
      Serial.println("RTC lost power - seeding from compile time.");
      setRtcFromCompileTime();
    }
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

  if (!readerSerial.available()) return;

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
