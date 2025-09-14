#include <NimBLEDevice.h>
#include "ClickDetector.h"

// ===== Pin Definitions =====
const int waterPin = 13;        // Water pump/valve
const int stepPin = 33;         // Stepper motor step
const int dirPin = 25;          // Stepper motor direction
const int enPin = 26;           // Stepper motor enable
const int vibrationPin = 32;    // Vibration motor
const int ledPin = 2;           // Built-in LED
const int barkButtonPin = 12;   // Bark trigger (button for testing)
const int waterButtonPin = 14;  // Manual water trigger
const int feederButtonPin = 27; // Manual feeder trigger
const int rfRemotePin = 35;     // Remote control trigger

// ===== BLE Configuration =====
#define ADV_NAME                "PING-ESP32" // Must match sender's ADV_NAME
#define ADV_TAG                 "PING1234"   // Must match sender's ADV_TAG
#define SCAN_INTERVAL_UNITS     80           // Scan interval (50 ms, units of 0.625 ms)
#define SCAN_WINDOW_UNITS       80           // Scan window (50 ms, units of 0.625 ms)
#define SCAN_DURATION_SECONDS   0            // 0 = continuous scan
#define SERIAL_BAUD_RATE        115200       // Serial baud rate

// ===== Training Logic Configuration =====
#define WATER_DURATION_MS       1000         // Water activation duration
#define VIBRATION_DURATION_MS   1000         // Vibration activation duration
#define STEPS_PER_TREAT         200          // Steps for one treat (adjust for your feeder)
#define STEP_PULSE_MS           2            // Pulse width for stepper (ms)
#define INITIAL_NO_BARK_MS      5000         // Initial no-bark interval (5s)
#define NO_BARK_INCREMENT_MS    5000         // Increase interval by 5s per reward
#define MAX_NO_BARK_MS          30000        // Max no-bark interval (30s)
#define DEBOUNCE_MS             50           // Button debounce time
#define LED_BLINK_MS            500          // LED blink interval when active

// ===== Punishment System State =====
struct PunishmentState {
    bool active;
    unsigned long startTime;
    String trigger;
    
    void start(const String& triggerSource) {
        if (!active) {  // Only start if not already active
            active = true;
            startTime = millis();
            trigger = triggerSource;
            
            // Activate punishment devices
            digitalWrite(waterPin, HIGH);
            digitalWrite(vibrationPin, HIGH);
            digitalWrite(ledPin, HIGH);
            
            Serial.printf("🚨 PUNISHMENT STARTED - Trigger: %s\n", trigger.c_str());
            Serial.println("   Water: ON, Vibration: ON, LED: ON");
        } else {
            Serial.printf("🚨 PUNISHMENT ALREADY ACTIVE - Ignoring trigger: %s\n", triggerSource.c_str());
        }
    }
    
    void stop() {
        if (active) {
            active = false;
            
            // Deactivate punishment devices
            digitalWrite(waterPin, LOW);
            digitalWrite(vibrationPin, LOW);
            digitalWrite(ledPin, LOW);
            
            unsigned long duration = millis() - startTime;
            Serial.printf("✅ PUNISHMENT STOPPED - Trigger: %s, Duration: %lu ms\n", trigger.c_str(), duration);
            Serial.println("   Water: OFF, Vibration: OFF, LED: OFF");
            
            trigger = "";
        }
    }
    
    bool shouldStop() {
        return active && (millis() - startTime >= WATER_DURATION_MS);
    }
    
    void update() {
        if (shouldStop()) {
            stop();
        }
    }
    
    bool isActive() {
        return active;
    }
};

// ===== Global Variables =====
unsigned long lastBarkTime = 0;      // Last bark detection time
unsigned long currentNoBarkInterval = INITIAL_NO_BARK_MS; // Current no-bark threshold
unsigned long lastWaterButtonTime = 0;   // Last water button press
unsigned long lastFeederButtonTime = 0; // Last feeder button press
unsigned long lastLedBlinkTime = 0;     // Last LED blink toggle
bool ledState = false;                   // LED state for blinking

// Replace pingActive and pingActivatedTime with unified punishment system
PunishmentState punishment = {false, 0, ""};

NimBLEScan* pBLEScan;
ClickDetector detector(35);  // GPIO35 for receiver

// BLE Callback Class
class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
    std::string deviceName = advertisedDevice->getName();
    if (deviceName == ADV_NAME) {
      std::string mfgData = advertisedDevice->getManufacturerData();
      if (mfgData.find(ADV_TAG) != std::string::npos) {
        
        // Use unified punishment system
        punishment.start("BLE Bark Detection");
        
        // Optional: Print BLE details (only when punishment starts)
        if (punishment.isActive()) {
          Serial.printf("📱 BLE Device: %s, RSSI: %d dBm\n", 
                       deviceName.c_str(), advertisedDevice->getRSSI());
        }
      }
    }
  }
};

// Debounce function for buttons
bool isButtonPressed(int pin, unsigned long& lastPressTime) {
  if (digitalRead(pin) == LOW) { // Assuming active-low buttons
    unsigned long currentTime = millis();
    if (currentTime - lastPressTime > DEBOUNCE_MS) {
      lastPressTime = currentTime;
      return true;
    }
  }
  return false;
}

// Dispense treat using stepper motor
void dispenseTreat() {
  digitalWrite(enPin, LOW); // Enable stepper
  digitalWrite(dirPin, HIGH); // Set direction (adjust as needed)
  for (int i = 0; i < STEPS_PER_TREAT; i++) {
    digitalWrite(stepPin, HIGH);
    delay(STEP_PULSE_MS);
    digitalWrite(stepPin, LOW);
    delay(STEP_PULSE_MS);
  }
  digitalWrite(enPin, HIGH); // Disable stepper
  Serial.println("🍖 Treat dispensed!");
}

// Initialize BLE scanning
void initBLEScan() {
  Serial.println("📡 BLE initialization started.");
  
  // Best practice configurations
  NimBLEDevice::setScanDuplicateCacheSize(200);
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);
  
  pBLEScan = NimBLEDevice::getScan();
  
  // Set both types of callbacks
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(SCAN_INTERVAL_UNITS);
  pBLEScan->setWindow(SCAN_WINDOW_UNITS);
  pBLEScan->setMaxResults(0); // Critical: don't store results
  
  Serial.println("✅ BLE scan configured successfully.");
}

// ===== REUSABLE PUNISHMENT AND REWARD FUNCTIONS =====

// Unified punishment function - used by all triggers
void triggerPunishment(const String& source) {
    punishment.start(source);
}

// Unified reward function
void triggerReward(const String& source) {
    Serial.printf("🎉 REWARD - Trigger: %s\n", source.c_str());
    dispenseTreat();
}

// Remote control actions - NOW USING UNIFIED SYSTEM
void remotePunishAction() {
    triggerPunishment("Remote Control");
}

void remoteTreatAction() {
    triggerReward("Remote Control");
}

void setup() {
  // Initialize pins
  pinMode(waterPin, OUTPUT);
  pinMode(vibrationPin, OUTPUT);
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(barkButtonPin, INPUT_PULLUP); // Active-low button
  pinMode(waterButtonPin, INPUT_PULLUP); // Active-low button
  pinMode(feederButtonPin, INPUT_PULLUP); // Active-low button
  digitalWrite(waterPin, LOW);
  digitalWrite(vibrationPin, LOW);
  digitalWrite(enPin, HIGH); // Stepper disabled by default
  digitalWrite(ledPin, LOW);

  // Initialize Serial
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println("=================================");
  Serial.println("🐕 Smart Dog Training System v2.0");
  Serial.println("=================================");
  while (!Serial) delay(10);

  // Initialize punishment system
  punishment = {false, 0, ""};

  // Initialize remote control detector
  detector.begin();
  detector.setCallbacks(
    // Single click callback - Punishment
    []() {
      Serial.println("🎮 Remote Single Click: Punishment");
      remotePunishAction();
    },
    // Double click callback - Reward
    []() {
      Serial.println("🎮 Remote Double Click: Reward");
      remoteTreatAction();
    }
  );

  // Initialize BLE scanning
  initBLEScan();
  
  Serial.println("\n✅ System Ready!");
  Serial.println("📡 BLE: Automatic bark detection active");
  Serial.println("🎮 Remote: Single click = punishment, Double click = reward");
  Serial.println("🔘 Buttons: Manual water/feeder controls available");
  Serial.println("⏱️  Punishment duration: 1000ms");
  Serial.println();
}

void loop() {
  unsigned long currentTime = millis();
  
  // Update remote control detector
  detector.update();
  
  // Update punishment system (handles timing automatically)
  punishment.update();
  
  // Check and restart scan if needed (improved BLE management)
  if (!pBLEScan->isScanning()) {
    Serial.println("📡 Starting/Restarting BLE scan...");
    pBLEScan->start(0, nullptr, false); // Non-blocking continuous scan
  }

  // Handle manual water button - USES UNIFIED SYSTEM
  if (isButtonPressed(waterButtonPin, lastWaterButtonTime)) {
    Serial.println("🔧 Manual water button pressed");
    triggerPunishment("Manual Water Button");
  }

  // Handle manual feeder button
  if (isButtonPressed(feederButtonPin, lastFeederButtonTime)) {
    Serial.println("🔧 Manual feeder button pressed");
    triggerReward("Manual Feeder Button");
  }

  // Handle bark detection button - USES UNIFIED SYSTEM
  if (isButtonPressed(barkButtonPin, lastBarkTime)) {
    Serial.println("🐕 Bark button pressed");
    triggerPunishment("Bark Detection Button");
    currentNoBarkInterval = INITIAL_NO_BARK_MS; // Reset interval
  }

  // Check for no-bark reward - ONLY when not punishing
  if (!punishment.isActive() && (currentTime - lastBarkTime >= currentNoBarkInterval)) {
    Serial.printf("🏆 No bark for %lu ms - rewarding good behavior\n", currentNoBarkInterval);
    triggerReward("No-Bark Timer");
    currentNoBarkInterval += NO_BARK_INCREMENT_MS; // Increase interval
    if (currentNoBarkInterval > MAX_NO_BARK_MS) {
      currentNoBarkInterval = MAX_NO_BARK_MS; // Cap at max
    }
    lastBarkTime = currentTime; // Reset timer after reward
  }

  // Blink LED to indicate system is active (when not punishing)
  if (!punishment.isActive() && (currentTime - lastLedBlinkTime >= LED_BLINK_MS)) {
    ledState = !ledState;
    digitalWrite(ledPin, ledState);
    lastLedBlinkTime = currentTime;
  }

  // Handle serial commands for debugging
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    
    if (cmd == "status") {
      String detectorStatus;
      detector.getStatus(detectorStatus);
      Serial.println("\n📊 SYSTEM STATUS:");
      Serial.printf("   Remote Detector: %s\n", detectorStatus.c_str());
      Serial.printf("   BLE Scan: %s\n", pBLEScan->isScanning() ? "Active" : "Stopped");
      Serial.printf("   Punishment Active: %s\n", punishment.isActive() ? "YES" : "NO");
      if (punishment.isActive()) {
        unsigned long elapsed = millis() - punishment.startTime;
        Serial.printf("   Punishment Source: %s\n", punishment.trigger.c_str());
        Serial.printf("   Punishment Elapsed: %lu ms\n", elapsed);
      }
      Serial.printf("   No-bark Interval: %lu ms\n", currentNoBarkInterval);
      Serial.printf("   Time Since Last Bark: %lu ms\n", millis() - lastBarkTime);
      Serial.println();
    }
    else if (cmd == "reset") {
      detector.reset();
      punishment.stop();  // Stop any active punishment
      Serial.println("🔄 System reset - Remote detector cleared, punishment stopped");
    }
    else if (cmd == "punish") {
      Serial.println("🧪 Manual punishment test");
      triggerPunishment("Serial Command");
    }
    else if (cmd == "reward") {
      Serial.println("🧪 Manual reward test");
      triggerReward("Serial Command");
    }
    else if (cmd == "stop") {
      punishment.stop();
      Serial.println("🛑 Punishment manually stopped");
    }
    else if (cmd == "help") {
      Serial.println("\n📖 AVAILABLE COMMANDS:");
      Serial.println("status  - Show detailed system status");
      Serial.println("reset   - Reset remote detector and stop punishment");
      Serial.println("punish  - Test punishment system");
      Serial.println("reward  - Test reward system");  
      Serial.println("stop    - Manually stop active punishment");
      Serial.println("help    - Show this help menu");
      Serial.println();
    }
    else if (cmd != "") {
      Serial.println("❓ Unknown command. Type 'help' for available commands.");
    }
  }
}

/*
===========================================
🎯 KEY IMPROVEMENTS:
===========================================

✅ UNIFIED PUNISHMENT SYSTEM:
   - All triggers (BLE, Remote, Buttons) use same logic
   - Consistent timing and behavior
   - Prevents overlapping punishments
   - Automatic start/stop management

✅ REUSABLE FUNCTIONS:
   - triggerPunishment(source) - Universal punishment
   - triggerReward(source) - Universal reward
   - Easy to add new triggers

✅ IMPROVED STATE MANAGEMENT:
   - PunishmentState struct handles all timing
   - Clear logging of trigger sources
   - Prevents double-activation

✅ ENHANCED DEBUGGING:
   - Detailed status command
   - Manual test commands
   - Better logging with trigger sources

✅ CONSISTENT BEHAVIOR:
   - Remote single click = punishment (same as bark detection)
   - Remote double click = reward (same as treat dispenser)
   - Manual buttons now use unified system

===========================================
*/