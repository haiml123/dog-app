#ifndef CLICK_DETECTOR_H
#define CLICK_DETECTOR_H

#include <Arduino.h>
#include "driver/rmt.h"
#include "driver/gpio.h"
#include <functional>

// Callback function types
typedef std::function<void()> ClickCallback;

class ClickDetector {
public:
    // Constructor - removed longPressMs, added tripleClickMs
    ClickDetector(int rxPin = 35, int doubleClickMs = 600, int debounceMs = 50, int tripleClickMs = 900);

    // Setup functions
    void begin();
    void setCallbacks(ClickCallback singleClick, ClickCallback doubleClick, ClickCallback tripleClick);

    // Main loop function - call this in your loop()
    void update();

    // Control functions
    void reset();
    bool isLearned();
    void getStatus(String& statusMsg);

    // Advanced settings
    void setDoubleClickTime(int ms);
    void setTripleClickTime(int ms);
    void setDebounceTime(int ms);
    void setMinPulses(int min);
    void setMaxPulses(int max);

private:
    // Hardware config
    int rxPin;
    rmt_channel_t rmtChannel;

    // Timing config
    int doubleClickMs;
    int tripleClickMs;
    int debounceMs;
    int minPulses;
    int maxPulses;

    // Button signature
    struct ButtonSignature {
        int minPulses;
        int maxPulses;
        int avgPulses;
        int sampleCount;
    } signature;

    bool hasSignature;

    // Click state - updated for triple click
    unsigned long lastPress;
    unsigned long firstClickTime;
    unsigned long secondClickTime;
    int clickCount;  // Track number of clicks (0, 1, 2, 3)

    // Callbacks
    ClickCallback singleClickCallback;
    ClickCallback doubleClickCallback;
    ClickCallback tripleClickCallback;

    // Internal functions
    void setupRMT();
    int readPulseCount();
    void updateSignature(int pulses);
    bool matchesSignature(int pulses);
    void handleButtonPress(int pulses);
    void processSignal();
};

#endif