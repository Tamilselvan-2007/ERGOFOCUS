#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// --- PIN DEFINITIONS (ESP32) ---
#define PIN_TRIG        5
#define PIN_ECHO        18
#define PIN_IR_SENSOR   27
#define PIN_BUZZER      23

#define PIN_LED_GREEN   2
#define PIN_LED_YELLOW  4
#define PIN_LED_RED     15

// --- CONSTANTS & CONFIGURATION ---
#define FOCUS_TIME_SEC  40 // 25 Minutes
#define BREAK_TIME_SEC  10  // 5 Minutes
#define HOLD_TIME_CALIB 4000      // 4 Seconds hold for calibration
#define SLOUCH_ALERT_MS 5000     // 15 Seconds continuous slouch warning

// --- SYSTEM STATES ---
enum SystemState {
  STATE_CALIB_UPRIGHT,
  STATE_CALIB_SLOUCH,
  STATE_READY,
  STATE_FOCUS,
  STATE_BREAK
};

// --- GLOBAL OBJECTS & VARIABLES ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

SystemState currentState = STATE_READY;

float idealDistance = 50.0;  // Default fallback (cm)
float slouchDistance = 30.0; // Default fallback (cm)
float thresholdDistance = 35.0;

unsigned long irPressStartTime = 0;
bool isIrPressed = false;
bool holdHandled = false;

unsigned long sessionStartTime = 0;
unsigned long slouchStartTime = 0;
bool isSlouching = false;
unsigned long lastSensorReadTime = 0;

float lastReadDistance = 0.0; // NEW: stores the most recent distance reading for live display

// --- FUNCTION DECLARATIONS ---
float readUltrasonicDistance();
void updateLEDs(bool green, bool yellow, bool red);
void playChime();
void playWarningChip();
void loadCalibration();
void saveCalibration();
String formatDistance(float d); // NEW: helper to format distance text consistently

void setup() {
  Serial.begin(115200);

  // Initialize Hardware Pins
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_IR_SENSOR, INPUT_PULLUP); // Active LOW for IR detection
  pinMode(PIN_BUZZER, OUTPUT);

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);

  // Initialize LCD
  lcd.init();
  lcd.backlight();

  // Load Saved Thresholds from ESP32 Flash Memory
  loadCalibration();

  // Boot into READY state
  currentState = STATE_READY;
  updateLEDs(false, true, false); // Yellow LED ON
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ErgoFocus Ready ");
  lcd.setCursor(0, 1);
  lcd.print("Wave Hand Start ");
}

void loop() {
  unsigned long currentMillis = millis();

  // --- 1. TIME-DOMAIN IR GESTURE FILTERING ---
  bool irActive = (digitalRead(PIN_IR_SENSOR) == LOW); // LOW = Hand present

  if (irActive && !isIrPressed) {
    isIrPressed = true;
    irPressStartTime = currentMillis;
    holdHandled = false;
  } 
  else if (irActive && isIrPressed) {
    // Check for 4-Second Hold (Triggers Calibration)
    if (!holdHandled && (currentMillis - irPressStartTime >= HOLD_TIME_CALIB)) {
      holdHandled = true;
      currentState = STATE_CALIB_UPRIGHT;
      lcd.clear();
      lcd.print("CALIBRATION MODE");
      lcd.setCursor(0, 1);
      lcd.print("Sit Up! Wave Hand");
      playChime();
    }
  } 
  else if (!irActive && isIrPressed) {
    isIrPressed = false;
    unsigned long pressDuration = currentMillis - irPressStartTime;

    // Quick Wave (< 1 sec) Action
    if (!holdHandled && pressDuration < 1000) {
      if (currentState == STATE_READY || currentState == STATE_BREAK) {
        // Start Focus Session
        currentState = STATE_FOCUS;
        sessionStartTime = currentMillis;
        slouchStartTime = 0;
        isSlouching = false;
        lastSensorReadTime = 0;       // force an immediate distance read on entry
        lastReadDistance = 0.0;
        updateLEDs(true, false, false); // Green LED ON
        lcd.clear();
      } 
      else if (currentState == STATE_CALIB_UPRIGHT) {
        idealDistance = readUltrasonicDistance();
        currentState = STATE_CALIB_SLOUCH;

        // NEW: Clearly show the measured upright distance to the user
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Upright: " + formatDistance(idealDistance));
        lcd.setCursor(0, 1);
        lcd.print("Now Slouch+Wave");
        playChime();
        delay(1800); // give the user time to read the number before it moves on
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("CALIBRATION MODE");
        lcd.setCursor(0, 1);
        lcd.print("Slouch & Wave...");
      } 
      else if (currentState == STATE_CALIB_SLOUCH) {
        slouchDistance = readUltrasonicDistance();
        saveCalibration();
        currentState = STATE_READY;
        updateLEDs(false, true, false); // Yellow LED ON

        // NEW: Show slouch distance and the final calculated threshold clearly
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Slouch: " + formatDistance(slouchDistance));
        lcd.setCursor(0, 1);
        lcd.print("Thresh: " + formatDistance(thresholdDistance));
        delay(2500); // hold this summary on screen long enough to read

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("ErgoFocus Ready ");
        lcd.setCursor(0, 1);
        lcd.print("Wave Hand Start ");
      }
    }
  }

  // --- 2. STATE MACHINE LOGIC ---
  switch (currentState) {
    
    case STATE_FOCUS: {
      unsigned long elapsedSec = (currentMillis - sessionStartTime) / 1000;
      if (elapsedSec >= FOCUS_TIME_SEC) {
        // Focus Ended -> Transition to Break
        currentState = STATE_BREAK;
        sessionStartTime = currentMillis;
        updateLEDs(false, true, false); // Yellow LED ON
        playChime();
        lcd.clear();
        break;
      }

      int remainingSec = FOCUS_TIME_SEC - elapsedSec;
      int mins = remainingSec / 60;
      int secs = remainingSec % 60;

      // Monitor Posture every 500ms (this also refreshes the live distance reading)
      if (currentMillis - lastSensorReadTime >= 500) {
        lastSensorReadTime = currentMillis;
        lastReadDistance = readUltrasonicDistance();

        if (lastReadDistance < thresholdDistance && lastReadDistance > 5.0) {
          if (!isSlouching) {
            isSlouching = true;
            slouchStartTime = currentMillis;
          } else if (currentMillis - slouchStartTime >= SLOUCH_ALERT_MS) {
            // Slouch Warning Triggered
            updateLEDs(false, false, true); // Red LED ON
            lcd.setCursor(0, 1);
            lcd.print("! FIX POSTURE ! ");
            playWarningChip();
          }
        } else {
          isSlouching = false;
          slouchStartTime = 0;
          updateLEDs(true, false, false); // Green LED ON
          lcd.setCursor(0, 1);
          lcd.print("Posture: GOOD   ");
        }
      }

      // NEW: Top line now shows LIVE distance (left corner) + countdown timer (right side)
      lcd.setCursor(0, 0);
      lcd.print(formatDistance(lastReadDistance)); // e.g. "34cm" - left corner

      lcd.setCursor(9, 0);
      if (mins < 10) lcd.print("0");
      lcd.print(mins);
      lcd.print(":");
      if (secs < 10) lcd.print("0");
      lcd.print(secs);

      break;
    }

    case STATE_BREAK: {
      unsigned long elapsedSec = (currentMillis - sessionStartTime) / 1000;
      if (elapsedSec >= BREAK_TIME_SEC) {
        // Break Completed -> Transition to Ready State (Post-Break Rule)
        currentState = STATE_READY;
        updateLEDs(false, true, false); // Solid Yellow LED
        
        // End-of-break alert
        playChime();
        for(int i = 0; i < 3; i++) {
          digitalWrite(PIN_LED_YELLOW, LOW);
          delay(150);
          digitalWrite(PIN_LED_YELLOW, HIGH);
          delay(150);
        }

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Break Completed!");
        lcd.setCursor(0, 1);
        lcd.print("Wave Hand Start ");
        break;
      }

      int remainingSec = BREAK_TIME_SEC - elapsedSec;
      int mins = remainingSec / 60;
      int secs = remainingSec % 60;

      // Update LCD Display
      lcd.setCursor(0, 0);
      lcd.print("BREAK   ");
      if (mins < 10) lcd.print("0");
      lcd.print(mins);
      lcd.print(":");
      if (secs < 10) lcd.print("0");
      lcd.print(secs);
      lcd.print(" ");

      lcd.setCursor(0, 1);
      lcd.print("Rest & Stretch! ");
      break;
    }

    default:
      break;
  }
}

// --- HELPER FUNCTIONS ---
float readUltrasonicDistance() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duration = pulseIn(PIN_ECHO, HIGH, 30000); // 30ms timeout
  if (duration == 0) return 100.0; // Default safe distance if out of range
  return duration * 0.0343 / 2.0;
}

void updateLEDs(bool green, bool yellow, bool red) {
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
  digitalWrite(PIN_LED_YELLOW, yellow ? HIGH : LOW);
  digitalWrite(PIN_LED_RED, red ? HIGH : LOW);
}

void playChime() {
  tone(PIN_BUZZER, 1000, 100);
  delay(120);
  tone(PIN_BUZZER, 1500, 150);
}

void playWarningChip() {
  tone(PIN_BUZZER, 2000, 80);
}

void loadCalibration() {
  prefs.begin("ergofocus", true);
  idealDistance = prefs.getFloat("ideal", 50.0);
  slouchDistance = prefs.getFloat("slouch", 30.0);
  prefs.end();

  thresholdDistance = slouchDistance + 4.0;
}

void saveCalibration() {
  prefs.begin("ergofocus", false);
  prefs.putFloat("ideal", idealDistance);
  prefs.putFloat("slouch", slouchDistance);
  prefs.end();

  thresholdDistance = slouchDistance + 4.0;
}

// NEW: Formats a distance value into a short, fixed-width, padded string like "34cm" or "100cm"
// Keeps LCD columns aligned so old digits don't linger on screen after updates.
String formatDistance(float d) {
  int rounded = (int)(d + 0.5);
  String s = String(rounded) + "cm";
  while (s.length() < 6) s += " "; // pad to 6 chars so it always overwrites cleanly
  return s;
}
