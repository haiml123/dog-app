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
    // Constructor
    ClickDetector(int rxPin = 35, int doubleClickMs = 600, int debounceMs = 50);

    // Setup functions
    void begin();
    void setCallbacks(ClickCallback singleClick, ClickCallback doubleClick);

    // Main loop function - call this in your loop()
    void update();

    // Control functions
    void reset();
    bool isLearned();
    void getStatus(String& statusMsg);

    // Advanced settings
    void setDoubleClickTime(int ms);
    void setDebounceTime(int ms);
    void setMinPulses(int min);
    void setMaxPulses(int max);

private:
    // Hardware config
    int rxPin;
    rmt_channel_t rmtChannel;

    // Timing config
    int doubleClickMs;
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

    // Click state
    unsigned long lastPress;
    unsigned long firstClickTime;
    bool waitingForDouble;

    // Callbacks
    ClickCallback singleClickCallback;
    ClickCallback doubleClickCallback;

    // Internal functions
    void setupRMT();
    int readPulseCount();
    void updateSignature(int pulses);
    bool matchesSignature(int pulses);
    void handleButtonPress(int pulses);
    void processSignal();
};

#endif