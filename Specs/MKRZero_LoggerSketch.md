# MKR Zero Bring-Up Logger Sketch

File: [Code/MKRZeroPrototype.C](../Code/MKRZeroPrototype.C)

This is a standalone bench sketch for the MKR Zero bring-up prototype (see
[MKRZero_Prototype_Wiring.md](MKRZero_Prototype_Wiring.md)). It is **not**
linked with the Artemis field firmware (`DataLogging.C`) — it exists to
validate WL-134 reads and tag decoding before that logic is ported over.

## What it does
1. Reads WL-134 frames over `Serial1` (9600 baud).
2. Validates the checksum (`checksum XOR inverted_checksum == 0xFF`).
3. Extracts the tag code into `CCC.NNNNNNNNNN` format.
4. Appends `millis(),tag` as a CSV line to `rfidlog.csv` on the microSD card.
5. Pulses the WL-134 `RST` pin periodically so a tag sitting in the field
   (not swimming away) gets re-read instead of only reading on first
   detection.

## Bugs found in the original bench sketch (from the chat transcript)

The original sketch produced no log file and no errors. Two issues combined
to cause this:

### 1. Fixed-interval reset collided with in-progress frame reads
The original `loop()` pulsed `RST` every 1000 ms unconditionally, with no
awareness of whether the WL-134 was mid-transmission. A reset pulse during a
frame transmission causes the WL-134 to abort output partway through, so the
sketch's `while (Serial1.available())` loop never sees an ETX (`0x03`) byte to
close the frame out cleanly, and reads a partial/garbled packet (or the
checksum bytes land on the wrong offset).

**Fix:** reset is now only triggered when no read is in progress
(`readInProgress` flag), and frame capture (`readFrame()`) runs independently
with its own timeout, so resets no longer truncate a frame mid-flight.

### 2. Every failure path returned silently
Both the "frame too short" and "checksum failed" branches did a bare
`return;` with no `Serial.print`. Combined with bug #1 (which made failures
the common case, not the exception), this meant **every** tag attempt failed
validation and silently discarded the frame — so `logCSV()` (and therefore
`SD.open(..., FILE_WRITE)`) was simply never reached, and no file was ever
created. There was nothing printed to reveal this because the failure paths
were silent, which is exactly the "no errors reported" symptom.

**Fix:** every discard path (`incomplete frame`, `checksum failed`, `tag
window too short`, `SD.open failed`) now prints a `WARN:`/`ERROR:` line to
`Serial`, so a bench run with the monitor open immediately shows why nothing
is being logged.

### Secondary checks worth confirming on hardware
- Confirm `SD.begin(SD_CS)` actually printed `"SD Card ready."` during the
  failing run — if it printed the init-failure message instead, the SD card
  itself (format/seating) is the root cause, independent of bugs #1/#2.
  Reformat the card FAT32 and re-seat it before re-testing.
- Confirm the WL-134 TX line is truly 3.3V logic before leaving it tied
  directly to pin 13 — see the open wiring issue in
  [MKRZero_Prototype_Wiring.md](MKRZero_Prototype_Wiring.md).

## Tag decode simplification
The transcript arrived at the extraction rule through a lot of trial and
error, but the final, confirmed rule is simple:

> Take the first 13 characters of the ASCII payload (after STX), reverse
> them character-by-character, then insert a `.` after the first 3
> characters.

`Code/MKRZeroPrototype.C` implements exactly this in one pass using a fixed
`char[]` buffer instead of the original's nested `String` operations
(`substring()` of a `substring()`, multiple `+=` reallocations). This avoids
repeated heap allocation from Arduino's `String` class, which the project
convention in [AGENTS.md](../AGENTS.md) calls out as a risk for any code that
will run continuously during a deployment (heap fragmentation over a 2-week
run). `extractTagID()` now:
- Operates directly on the raw `char *frame` buffer.
- Does the reversal and period insertion in a single loop, writing straight
  into the output buffer.
- Returns `false` (instead of silently producing a truncated/garbage tag) if
  the frame is shorter than the expected 13-character tag window.

## Timestamps
`rfidlog.csv` rows are logged as `YYYY-MM-DD HH:MM:SS,tag` using the SAMD21's
onboard RTC peripheral (via `RTCZero`), seeded from the sketch's compile time
(`__DATE__`/`__TIME__`) in `setRtcFromCompileTime()`. Unlike the RV-1805 used
in the field design, this onboard RTC has **no battery backup** — it only
keeps correct time while the board stays continuously powered, and resets
whenever power is lost or the board is re-flashed. Re-flashing re-seeds it to
the new compile time, so dates stay roughly current across bench sessions,
but a long field deployment with a power interruption would need the time
re-set over Serial rather than relying on compile-time seeding.
