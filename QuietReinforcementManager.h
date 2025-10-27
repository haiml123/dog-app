#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct LevelConfig {
  uint32_t quietMs;          // How long the dog must be quiet
  uint32_t dispenseMs;       // How long to run the feeder
  const uint8_t* pattern;    // Punch-card pattern, e.g. {1,1,1,1} for 100%
  uint8_t patternLen;        // Length of pattern
  bool shuffleEachCycle;     // Shuffle pattern after it's consumed? (use for VR)
};

class QuietReinforcementManager {
public:
  QuietReinforcementManager(const char* nvsNamespace,
                            const LevelConfig* levels,
                            uint8_t levelCount,
                            uint8_t successesToAdvance = 4,
                            uint32_t minDispenseCooldownMs = 7000,
                            uint8_t levelsToDemoteOnBark = 0,
                            bool enableLog = false)
  : _ns(nvsNamespace),
    _levels(levels),
    _levelCount(levelCount),
    _needSuccesses(successesToAdvance),
    _cooldownMs(minDispenseCooldownMs),
    _logEnabled(enableLog),
    _demotionLevels(levelsToDemoteOnBark) {}  // NEW

  // Enable/disable logs dynamically
  void setLogging(bool enabled) { _logEnabled = enabled; }

  // NEW: Set how many levels to drop on bark (0 = no demotion)
  void setDemotionLevels(uint8_t levels) {
    _demotionLevels = levels;
    _log("Demotion levels set to: " + String(_demotionLevels));
  }

  // NEW: Get current demotion setting
  uint8_t getDemotionLevels() const { return _demotionLevels; }

  // Call at boot
  void begin() {
    _prefs.begin(_ns, false);
    _currentLevel = _prefs.getUChar("lvl", 0);
    if (_currentLevel >= _levelCount) _currentLevel = 0;

    _successesAtLevel = _prefs.getUChar("succ", 0);
    _patternIndex = _prefs.getUChar("pidx", 0);

    uint32_t now = millis();
    _quietStartMs = now;
    _lastBarkMs = now;
    _rewardCooldownUntil = 0;
    _pendingDispenseMs = 0;

    // Seed RNG for shuffling
    uint32_t seed = esp_random();
    randomSeed(seed);

    _log("Initialized. Level=" + String(_currentLevel) + ", Demotion=" + String(_demotionLevels));
  }

  // Call when bark/noise is detected - NOW WITH DEMOTION
  void onBark(uint32_t nowMs) {
    _lastBarkMs = nowMs;
    _successesAtLevel = 0;
    _quietStartMs = nowMs;
    _pendingDispenseMs = 0;

    // NEW: Apply level demotion if configured
    if (_demotionLevels > 0 && _currentLevel > 0) {
      uint8_t oldLevel = _currentLevel;

      // Drop levels, but never go below 0
      if (_currentLevel >= _demotionLevels) {
        _currentLevel -= _demotionLevels;
      } else {
        _currentLevel = 0;
      }

      // Reset pattern index when level changes
      _patternIndex = 0;

      if (oldLevel != _currentLevel) {
        _log("Bark detected. DEMOTED: Level " + String(oldLevel) +
             " → Level " + String(_currentLevel) + " (-" + String(oldLevel - _currentLevel) + ")");
      } else {
        _log("Bark detected. Reset quiet timer, level=" + String(_currentLevel));
      }
    } else {
      _log("Bark detected. Reset quiet timer, level=" + String(_currentLevel));
    }

    _saveThrottled(nowMs);
  }

  // Call frequently from loop(); returns true if it just decided a dispense
  bool tick(uint32_t nowMs) {
    if (_pendingDispenseMs > 0) return false;

    const LevelConfig& L = _levels[_currentLevel];

    if (nowMs - _quietStartMs >= L.quietMs) {
      bool shouldReward = _decideReinforcement(L);
      _successesAtLevel++;

      if (shouldReward && nowMs >= _rewardCooldownUntil) {
        _pendingDispenseMs = L.dispenseMs;
        _rewardCooldownUntil = nowMs + _cooldownMs;
        _log("Reward scheduled: " + String(_pendingDispenseMs) + "ms");
      } else {
        _log("Quiet success, no reward this time. Pattern idx=" + String(_patternIndex));
      }

      _quietStartMs = nowMs;

      if (_successesAtLevel >= _needSuccesses) {
        _currentLevel = (_currentLevel + 1 < _levelCount) ? _currentLevel + 1 : _currentLevel;
        _successesAtLevel = 0;
        _log("Level up! New level=" + String(_currentLevel));
      }

      _saveThrottled(nowMs);
      return _pendingDispenseMs > 0;
    }

    return false;
  }

  // If tick() decided to dispense, call this to fetch and clear the action
  uint32_t consumePendingDispenseMs() {
    uint32_t ms = _pendingDispenseMs;
    _pendingDispenseMs = 0;
    if (ms > 0) _log("Dispensing consumed: " + String(ms) + "ms");
    return ms;
  }

  // Allow manual level set / reset
  void setLevel(uint8_t lvl, uint32_t nowMs) {
    if (lvl >= _levelCount) return;
    _currentLevel = lvl;
    _successesAtLevel = 0;
    _patternIndex = 0;
    _quietStartMs = nowMs;
    _saveImmediate();
    _log("Level manually set to " + String(lvl));
  }

  // Reset state completely (to level 0, no successes, no dispense pending)
  void resetState() {
    uint32_t nowMs = millis();

    _currentLevel = 0;
    _successesAtLevel = 0;
    _patternIndex = 0;
    _quietStartMs = nowMs;
    _lastBarkMs = nowMs;
    _rewardCooldownUntil = 0;
    _pendingDispenseMs = 0;
    _lastSaveMs = 0;

    // Reset persisted values only (don't clear namespace)
    _prefs.putUChar("lvl", 0);
    _prefs.putUChar("succ", 0);
    _prefs.putUChar("pidx", 0);

    _log("State reset. Back to level 0.");
  }

  // Getters
  uint8_t  currentLevel() const        { return _currentLevel; }
  uint8_t  successesAtLevel() const    { return _successesAtLevel; }
  uint32_t currentQuietTargetMs() const{ return _levels[_currentLevel].quietMs; }
  uint32_t lastBarkMs() const          { return _lastBarkMs; }

private:
  bool _decideReinforcement(const LevelConfig& L) {
    if (L.patternLen == 0 || L.pattern == nullptr) return true;
    uint8_t value = L.pattern[_patternIndex];
    _patternIndex++;
    if (_patternIndex >= L.patternLen) {
      _patternIndex = 0;
      if (L.shuffleEachCycle) _shufflePattern(L);
    }
    return value != 0;
  }

  void _shufflePattern(const LevelConfig& L) {
    _patternIndex = random(0, L.patternLen);
    _log("Pattern reshuffled, new start idx=" + String(_patternIndex));
  }

  void _saveImmediate() {
    _prefs.putUChar("lvl",  _currentLevel);
    _prefs.putUChar("succ", _successesAtLevel);
    _prefs.putUChar("pidx", _patternIndex);
    _log("State saved: lvl=" + String(_currentLevel) + " succ=" + String(_successesAtLevel));
  }

  void _saveThrottled(uint32_t nowMs) {
    if (nowMs - _lastSaveMs >= 10000UL) {
      _lastSaveMs = nowMs;
      _saveImmediate();
    }
  }

  void _log(const String& msg) {
    if (_logEnabled) {
      Serial.print("[QuietReinforcement] ");
      Serial.println(msg);
    }
  }

  // --- State ---
  const char* _ns;
  Preferences _prefs;

  const LevelConfig* _levels;
  uint8_t _levelCount;

  uint8_t  _currentLevel{0};
  uint8_t  _successesAtLevel{0};
  uint8_t  _patternIndex{0};

  uint32_t _quietStartMs{0};
  uint32_t _lastBarkMs{0};
  uint32_t _rewardCooldownUntil{0};
  uint32_t _pendingDispenseMs{0};
  uint32_t _lastSaveMs{0};

  uint8_t  _needSuccesses{4};
  uint32_t _cooldownMs{7000};
  uint8_t  _demotionLevels{0};  // NEW: How many levels to drop on bark

  bool _logEnabled{false};
};