#include <NimBLEDevice.h>
#include "ClickDetector.h"
#include "QuietReinforcementManager.h"
#include "BLEBarkWindow"
// ===== Pin Definitions =====
const int waterPin = 13;
const int stepPin = 33;
const int dirPin = 25;
const int enPin = 26;
const int vibrationPin = 32;
const int ledPin = 2;
const int barkButtonPin = 12;
const int waterButtonPin = 14;   // Manual water/punishment (does NOT affect manager)
const int feederButtonPin = 27;  // Manual feeder/reward (does NOT affect manager)
const int rfRemotePin = 35;

// ===== BLE Configuration =====
#define ADV_NAME                "PING-ESP32"
#define ADV_TAG                 "PING1234"
#define SCAN_INTERVAL_UNITS     80
#define SCAN_WINDOW_UNITS       80
#define SCAN_DURATION_SECONDS   0
#define SERIAL_BAUD_RATE        115200

// ===== Hardware timings =====
#define STEP_PULSE_MS           2
#define DEBOUNCE_MS             50
#define LED_BLINK_MS            500

// ===== Manual action durations (do NOT affect manager) =====
#define MANUAL_PUNISH_MS        2000
#define MANUAL_REWARD_MS        1200
#define BARK_WINDOW             5000

// ===== REINFORCEMENT LEVELS (manager-driven rewards) =====
// Patterns: 1=reward, 0=skip
const uint8_t P_100[] = {1,1,1,1};         // 100%
const uint8_t P_80[]  = {1,1,1,1,0};       // ~80%
const uint8_t P_50[]  = {1,0};             // 50%

LevelConfig LEVELS[] = {
  // quietMs, dispenseMs, pattern,      len,             shuffle
  { 2000,     1200,       P_100,        sizeof(P_100),   false }, // L0: 2s, 100%
  { 4000,     1400,       P_100,        sizeof(P_100),   false }, // L1: 4s, 100%
  { 6000,     1600,       P_80,         sizeof(P_100),    false  }, // L2: 6s, ~80%
  { 9000,     1800,       P_80,         sizeof(P_100),    false  }, // L3: 9s, 50%
  { 12000,    2600,       P_80,         sizeof(P_100),    false  }, // L3: 9s, 50%
};
const uint8_t LEVEL_COUNT = sizeof(LEVELS)/sizeof(LEVELS[0]);

// Manager: (namespace, levels, count, successesToAdvance, rewardCooldownMs, log, punishmentMs)
QuietReinforcementManager quietMgr("dogNVS", LEVELS, LEVEL_COUNT, 4, 7000, 3, true);

BLEBarkWindow bleBarkWindow(BARK_WINDOW);  // 5 second window

// ===== Button debounce state =====
unsigned long lastWaterButtonTime = 0;
unsigned long lastFeederButtonTime = 0;
unsigned long lastBarkButtonTime  = 0;
unsigned long lastLedBlinkTime    = 0;
bool ledState = false;

// ===== Non-blocking punishment runner =====
bool punishActive = false;
unsigned long punishEndMs = 0;

// ===== BLE =====
NimBLEScan* pBLEScan;
ClickDetector detector(rfRemotePin);  // GPIO35

// Debounce
bool isButtonPressed(int pin, unsigned long& lastPressTime) {
  if (digitalRead(pin) == LOW) { // active-low
    unsigned long now = millis();
    if (now - lastPressTime > DEBOUNCE_MS) {
      lastPressTime = now;
      return true;
    }
  }
  return false;
}

// Feeder: run for durationMs (simple loop; convert to FSM if needed)
void runFeederFor(uint32_t durationMs) {
  digitalWrite(enPin, LOW);
  digitalWrite(dirPin, HIGH);
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    digitalWrite(stepPin, HIGH);
    delay(STEP_PULSE_MS);
    digitalWrite(stepPin, LOW);
    delay(STEP_PULSE_MS);
  }
  digitalWrite(enPin, HIGH);
  Serial.printf("🍖 Treat dispensed for %lu ms\n", (unsigned long)durationMs);
}

// Water: run pump/valve for durationMs (blocking)
void runWaterFor(uint32_t durationMs) {
  if (durationMs == 0) return;

  digitalWrite(waterPin, HIGH);
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    delay(1); // keep loop responsive-ish
  }
  digitalWrite(waterPin, LOW);
  Serial.printf("💧 Water ran for %lu ms\n", (unsigned long)durationMs);
}

// Manager-driven punishment (from barks)
void startPunishment(uint32_t ms) {
  if (ms == 0) return;
  punishActive = true;
  punishEndMs = millis() + ms;

  digitalWrite(waterPin, HIGH);
  digitalWrite(vibrationPin, HIGH);
  digitalWrite(ledPin, HIGH);
  Serial.printf("🚨 Punishment ON for %lu ms (manager)\n", (unsigned long)ms);
}

// Update punishment runner
void updatePunishment() {
  if (punishActive && (long)(millis() - punishEndMs) >= 0) {
    punishActive = false;
    digitalWrite(waterPin, LOW);
    digitalWrite(vibrationPin, LOW);
    digitalWrite(ledPin, LOW);
    Serial.println("✅ Punishment OFF");
  }
}

// BLE callbacks → on bark, notify manager (affects manager)
class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    std::string deviceName = d->getName();
    if (deviceName == ADV_NAME) {
      std::string mfgData = d->getManufacturerData();
      if (mfgData.find(ADV_TAG) != std::string::npos) {

        uint32_t now = millis();

        // Check window before punishing
        if (bleBarkWindow.shouldPunish(now)) {
          quietMgr.onBark(now);  // enqueue punishment + reset quiet window
          startPunishment(MANUAL_PUNISH_MS);
          Serial.printf("📱 BLE Bark Detected. RSSI: %d dBm\n", d->getRSSI());
        }
        // If shouldPunish returns false, bark is logged but ignored
      }
    }
  }
};

void initBLEScan() {
  Serial.println("📡 BLE initialization started.");
  NimBLEDevice::setScanDuplicateCacheSize(200);
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(SCAN_INTERVAL_UNITS);
  pBLEScan->setWindow(SCAN_WINDOW_UNITS);
  pBLEScan->setMaxResults(0);
  Serial.println("✅ BLE scan configured successfully.");
}

void setup() {
  // Pins
  pinMode(waterPin, OUTPUT);
  pinMode(vibrationPin, OUTPUT);
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(barkButtonPin, INPUT_PULLUP);
  pinMode(waterButtonPin, INPUT_PULLUP);
  pinMode(feederButtonPin, INPUT_PULLUP);
  digitalWrite(waterPin, LOW);
  digitalWrite(vibrationPin, LOW);
  digitalWrite(enPin, HIGH);
  digitalWrite(ledPin, LOW);

  // Serial
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println("=================================");
  Serial.println("🐕 Smart Dog Training System v3.1");
  Serial.println("=================================");
  while (!Serial) delay(10);

  // Remote click detector
  detector.begin();
  detector.setCallbacks(
    []() { // single click → manual punishment ONLY (does NOT affect manager)
      Serial.println("🎮 Remote Single Click → MANUAL punishment");
      startPunishment(MANUAL_PUNISH_MS);
    },
    []() { // double click → manual reward ONLY (does NOT affect manager)
      Serial.println("🎮 Remote Double Click → MANUAL reward");
      runFeederFor(MANUAL_REWARD_MS);
    },
     []() { // Long press
          Serial.println("🎮 Remote Triple Press Click → reset");
            quietMgr.resetState();
            Serial.println("🔄 QuietMgr reset");

            // First vibration pulse
            digitalWrite(vibrationPin, HIGH);
            delay(500);
            digitalWrite(vibrationPin, LOW);
            delay(500);
            digitalWrite(vibrationPin, HIGH);
            delay(500);
            digitalWrite(vibrationPin, LOW);
        }
  );

  // BLE
  initBLEScan();

  // Quiet manager
  quietMgr.begin();
  quietMgr.setLogging(true);

  Serial.println("\n✅ System Ready!");
  Serial.println("📡 BLE: Bark → manager (punish + reset quiet window)");
  Serial.println("🎮 Remote: Single→manual punish, Double→manual reward (no manager)");
  Serial.println("🔘 Water button→manual punish (no manager), Feeder button→manual reward (no manager)");
  Serial.println("🐕 Bark button→manager bark");
  Serial.println();
}

void loop() {
  uint32_t now = millis();

  // Remote
  detector.update();

  // Keep BLE scanning
  if (!pBLEScan->isScanning()) {
    pBLEScan->start(0, nullptr, false);
  }

  // === Inputs ===
  // Bark button → affects manager
  if (isButtonPressed(barkButtonPin, lastBarkButtonTime)) {
    Serial.println("🐕 Bark button pressed → manager bark");
    quietMgr.onBark(now);
     Serial.println("\n✅ Loop!");
  }

  // Water button → manual punishment ONLY (no manager)
  if (isButtonPressed(waterButtonPin, lastWaterButtonTime)) {
    Serial.println("🔧 Manual water button → MANUAL punishment");
    runWaterFor(MANUAL_REWARD_MS);
  }

  // Feeder button → manual reward ONLY (no manager)
  if (isButtonPressed(feederButtonPin, lastFeederButtonTime)) {
    Serial.println("🔧 Manual feeder button → MANUAL reward");
    runFeederFor(MANUAL_REWARD_MS);
  }

  // === Manager decisions ===
  // Rewards (quiet success)
  if (quietMgr.tick(now)) {
    uint32_t treatMs = quietMgr.consumePendingDispenseMs();
    if (treatMs > 0 && !punishActive) {
      Serial.printf("🏆 Manager reward: %lu ms\n", (unsigned long)treatMs);
      runFeederFor(treatMs);
    }
  }

  updatePunishment();

  // Blink LED when system is idle
  if (!punishActive && (now - lastLedBlinkTime >= LED_BLINK_MS)) {
    ledState = !ledState;
    digitalWrite(ledPin, ledState);
    lastLedBlinkTime = now;
  }

  // === Serial commands ===
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim(); cmd.toLowerCase();

    if (cmd == "status") {
      String detectorStatus;
      detector.getStatus(detectorStatus);
      Serial.println("\n📊 SYSTEM STATUS:");
      Serial.printf("   Remote Detector: %s\n", detectorStatus.c_str());
      Serial.printf("   BLE Scan: %s\n", pBLEScan->isScanning() ? "Active" : "Stopped");
      Serial.printf("   QuietMgr Level: %u\n", quietMgr.currentLevel());
      Serial.printf("   QuietMgr Successes: %u\n", quietMgr.successesAtLevel());
      Serial.printf("   Quiet Target: %lu ms\n", (unsigned long)quietMgr.currentQuietTargetMs());
      Serial.printf("   Last Bark: %lu ms ago\n\n", (unsigned long)(millis() - quietMgr.lastBarkMs()));
    }
    else if (cmd == "qreset") {
      quietMgr.resetState();
      Serial.println("🔄 QuietMgr reset");
    }
    else if (cmd.startsWith("qlevel")) {
      int lvl = cmd.substring(6).toInt();
      quietMgr.setLevel((uint8_t)lvl, millis());
      Serial.printf("🔧 QuietMgr level set to %d\n", lvl);
    }
    else if (cmd == "qlog on") {
      quietMgr.setLogging(true);  Serial.println("📝 QuietMgr logging: ON");
    }
    else if (cmd == "qlog off") {
      quietMgr.setLogging(false); Serial.println("📝 QuietMgr logging: OFF");
    }
    else if (cmd == "help") {
      Serial.println("\n📖 COMMANDS:");
      Serial.println("status     - Show system & QuietMgr status");
      Serial.println("qreset     - Reset QuietMgr (level=0)");
      Serial.println("qlevel X   - Manually set level");
      Serial.println("qlog on/off- Toggle QuietMgr logging");
      Serial.println();
    }
  }
}
