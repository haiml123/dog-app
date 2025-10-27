#pragma once
#include <Arduino.h>

// Minimal bark window - only for BLE callback
class BLEBarkWindow {
private:
  uint32_t _windowMs;
  uint32_t _lastPunishMs;
  uint32_t _suppressedCount;

public:
  BLEBarkWindow(uint32_t windowMs = 5000)
    : _windowMs(windowMs), _lastPunishMs(0), _suppressedCount(0) {}

  bool shouldPunish(uint32_t nowMs) {
    if (nowMs - _lastPunishMs < _windowMs) {
      _suppressedCount++;
      Serial.printf("⏸️  BLE Bark suppressed (#%d in window, %lu ms since last)\n",
                    _suppressedCount, nowMs - _lastPunishMs);
      return false;
    }

    if (_suppressedCount > 0) {
      Serial.printf("✅ Window expired. Suppressed %d barks.\n", _suppressedCount);
      _suppressedCount = 0;
    }

    _lastPunishMs = nowMs;
    return true;
  }

  void setWindow(uint32_t ms) { _windowMs = ms; }
  uint32_t getWindow() const { return _windowMs; }
  void reset() { _lastPunishMs = 0; _suppressedCount = 0; }
};