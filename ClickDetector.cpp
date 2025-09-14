#include "ClickDetector.h"

ClickDetector::ClickDetector(int rxPin, int doubleClickMs, int debounceMs) {
    this->rxPin = rxPin;
    this->doubleClickMs = doubleClickMs;
    this->debounceMs = debounceMs;
    this->rmtChannel = RMT_CHANNEL_0;
    this->minPulses = 50;
    this->maxPulses = 400;

    // Reset state
    hasSignature = false;
    waitingForDouble = false;
    lastPress = 0;
    firstClickTime = 0;
    signature = {0, 0, 0, 0};
}

void ClickDetector::begin() {
    pinMode(rxPin, INPUT);
    setupRMT();
    Serial.println("🎮 ClickDetector initialized");
}

void ClickDetector::setCallbacks(ClickCallback singleClick, ClickCallback doubleClick) {
    singleClickCallback = singleClick;
    doubleClickCallback = doubleClick;
}

void ClickDetector::setupRMT() {
    rmt_config_t config = {};
    config.rmt_mode = RMT_MODE_RX;
    config.channel = rmtChannel;
    config.gpio_num = (gpio_num_t)rxPin;
    config.clk_div = 80;
    config.mem_block_num = 2;
    config.rx_config.filter_en = true;
    config.rx_config.filter_ticks_thresh = 80;
    config.rx_config.idle_threshold = 12000;

    rmt_config(&config);
    rmt_driver_install(rmtChannel, 1024, 0);
    rmt_rx_start(rmtChannel, true);
}

int ClickDetector::readPulseCount() {
    RingbufHandle_t rb = nullptr;
    rmt_get_ringbuf_handle(rmtChannel, &rb);
    if (!rb) return 0;

    size_t length = 0;
    rmt_item32_t* items = (rmt_item32_t*)xRingbufferReceive(rb, &length, 10 / portTICK_PERIOD_MS);
    if (!items) return 0;

    int nItems = length / sizeof(rmt_item32_t);
    int pulseCount = 0;

    for (int i = 0; i < nItems && pulseCount < maxPulses; i++) {
        if (items[i].duration0 > 0) pulseCount++;
        if (items[i].duration1 > 0) pulseCount++;
    }

    vRingbufferReturnItem(rb, items);
    return pulseCount;
}

void ClickDetector::updateSignature(int pulses) {
    if (!hasSignature) {
        signature.minPulses = pulses;
        signature.maxPulses = pulses;
        signature.avgPulses = pulses;
        signature.sampleCount = 1;
        hasSignature = true;
        Serial.printf("📊 Initial signature: %d pulses\n", pulses);
    } else {
        signature.minPulses = min(signature.minPulses, pulses);
        signature.maxPulses = max(signature.maxPulses, pulses);
        signature.avgPulses = ((signature.avgPulses * signature.sampleCount) + pulses) / (signature.sampleCount + 1);
        signature.sampleCount++;

        if (signature.sampleCount <= 10) {
            Serial.printf("📊 Updated signature: %d-%d pulses (avg: %d, samples: %d)\n",
                         signature.minPulses, signature.maxPulses, signature.avgPulses, signature.sampleCount);
        }
    }
}

bool ClickDetector::matchesSignature(int pulses) {
    if (!hasSignature) return false;

    int range = signature.maxPulses - signature.minPulses;
    int tolerance = max(30, range + 20);

    int minAccepted = signature.avgPulses - tolerance;
    int maxAccepted = signature.avgPulses + tolerance;

    bool matches = (pulses >= minAccepted && pulses <= maxAccepted);

    if (!matches) {
        Serial.printf("❓ Out of range: %d (expected: %d±%d)\n", pulses, signature.avgPulses, tolerance);
    }

    return matches;
}

void ClickDetector::handleButtonPress(int pulses) {
    unsigned long now = millis();

    updateSignature(pulses);

    if (now - lastPress < debounceMs) {
        Serial.println("⚡ Debounced");
        return;
    }
    lastPress = now;

    if (waitingForDouble) {
        waitingForDouble = false;
        Serial.println("👆👆 DOUBLE CLICK");
        if (doubleClickCallback) doubleClickCallback();
    } else {
        waitingForDouble = true;
        firstClickTime = now;
        Serial.println("⏳ First click (waiting for double...)");
    }
}

void ClickDetector::processSignal() {
    int pulses = readPulseCount();
    if (pulses < minPulses) return;

    if (!hasSignature) {
        updateSignature(pulses);
        if (signature.sampleCount >= 3) {
            Serial.printf("✅ Button learned! Range: %d-%d pulses (avg: %d)\n",
                         signature.minPulses, signature.maxPulses, signature.avgPulses);
            Serial.println("🎮 Ready for single/double click detection!");
        } else {
            Serial.printf("📚 Learning... (%d/3 samples, %d pulses)\n", signature.sampleCount, pulses);
            Serial.println("   Press the SAME button again...");
        }
        return;
    }

    if (matchesSignature(pulses)) {
        Serial.printf("🎯 Button detected (%d pulses)!\n", pulses);
        handleButtonPress(pulses);
    } else {
        Serial.printf("❓ Different button (%d pulses) - ignored\n", pulses);
    }
}

void ClickDetector::update() {
    // Check single-click timeout
    if (waitingForDouble && (millis() - firstClickTime >= doubleClickMs)) {
        waitingForDouble = false;
        Serial.println("👆 SINGLE CLICK");
        if (singleClickCallback) singleClickCallback();
    }

    processSignal();
}

void ClickDetector::reset() {
    hasSignature = false;
    signature = {0, 0, 0, 0};
    waitingForDouble = false;
    Serial.println("🔄 ClickDetector reset");
}

bool ClickDetector::isLearned() {
    return hasSignature && signature.sampleCount >= 3;
}

void ClickDetector::getStatus(String& statusMsg) {
    if (isLearned()) {
        statusMsg = "✅ Learned: " + String(signature.minPulses) + "-" +
                   String(signature.maxPulses) + " pulses (avg: " +
                   String(signature.avgPulses) + ")";
    } else {
        statusMsg = "❌ Not learned yet (" + String(signature.sampleCount) + "/3 samples)";
    }
}

void ClickDetector::setDoubleClickTime(int ms) { doubleClickMs = ms; }
void ClickDetector::setDebounceTime(int ms) { debounceMs = ms; }
void ClickDetector::setMinPulses(int min) { minPulses = min; }
void ClickDetector::setMaxPulses(int max) { maxPulses = max; }