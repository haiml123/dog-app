#include "ClickDetector.h"

ClickDetector::ClickDetector(int rxPin, int doubleClickMs, int debounceMs, int tripleClickMs) {
    this->rxPin = rxPin;
    this->doubleClickMs = doubleClickMs;
    this->debounceMs = debounceMs;
    this->tripleClickMs = tripleClickMs;
    this->rmtChannel = RMT_CHANNEL_0;
    this->minPulses = 50;
    this->maxPulses = 400;

    // Reset state
    hasSignature = false;
    clickCount = 0;
    lastPress = 0;
    firstClickTime = 0;
    secondClickTime = 0;
    lastCallbackTime = 0;
    signature = {0, 0, 0, 0};
}

void ClickDetector::begin() {
    pinMode(rxPin, INPUT);
    setupRMT();
    Serial.println("ClickDetector initialized");
}

void ClickDetector::setCallbacks(ClickCallback singleClick, ClickCallback doubleClick, ClickCallback tripleClick) {
    singleClickCallback = singleClick;
    doubleClickCallback = doubleClick;
    tripleClickCallback = tripleClick;
}

void ClickDetector::setupRMT() {
    rmt_config_t config = {};
    config.rmt_mode = RMT_MODE_RX;
    config.channel = rmtChannel;
    config.gpio_num = (gpio_num_t)rxPin;
    config.clk_div = 80;
    config.mem_block_num = 4;  // FIXED: Was 2, now 4
    config.rx_config.filter_en = true;
    config.rx_config.filter_ticks_thresh = 100;  // FIXED: Was 80, now 100
    config.rx_config.idle_threshold = 15000;  // FIXED: Was 12000, now 15000

    rmt_config(&config);
    rmt_driver_install(rmtChannel, 2048, 0);  // FIXED: Was 1024, now 2048
    rmt_rx_start(rmtChannel, true);
}

// CRITICAL FIX: This function drains the buffer completely
int ClickDetector::readPulseCount() {
    RingbufHandle_t rb = nullptr;
    rmt_get_ringbuf_handle(rmtChannel, &rb);
    if (!rb) return 0;

    size_t length = 0;
    int lastValidPulseCount = 0;
    int itemsProcessed = 0;
    const int MAX_ITEMS_PER_CALL = 20;

    // DRAIN ALL ITEMS FROM BUFFER - this prevents "BUFFER FULL" errors
    while (itemsProcessed < MAX_ITEMS_PER_CALL) {
        rmt_item32_t* items = (rmt_item32_t*)xRingbufferReceive(rb, &length, 5 / portTICK_PERIOD_MS);

        if (!items) break;  // Buffer is empty

        int nItems = length / sizeof(rmt_item32_t);
        int pulseCount = 0;

        for (int i = 0; i < nItems && pulseCount < maxPulses; i++) {
            if (items[i].duration0 > 0) pulseCount++;
            if (items[i].duration1 > 0) pulseCount++;
        }

        vRingbufferReturnItem(rb, items);
        itemsProcessed++;

        // Keep only LAST valid signal
        if (pulseCount >= minPulses && pulseCount <= maxPulses) {
            lastValidPulseCount = pulseCount;
        }
    }

    // Warn if buffer had many items (RF noise)
    if (itemsProcessed > 10) {
        Serial.printf("[ClickDetector] Buffer had %d items\n", itemsProcessed);
    }

    return lastValidPulseCount;
}

void ClickDetector::updateSignature(int pulses) {
    if (!hasSignature) {
        signature.minPulses = pulses;
        signature.maxPulses = pulses;
        signature.avgPulses = pulses;
        signature.sampleCount = 1;
        hasSignature = true;
        Serial.printf("Initial signature: %d pulses\n", pulses);
    } else {
        signature.minPulses = min(signature.minPulses, pulses);
        signature.maxPulses = max(signature.maxPulses, pulses);
        signature.avgPulses = ((signature.avgPulses * signature.sampleCount) + pulses) / (signature.sampleCount + 1);
        signature.sampleCount++;

        if (signature.sampleCount <= 10) {
            Serial.printf("Updated signature: %d-%d pulses (avg: %d, samples: %d)\n",
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
        Serial.printf("Out of range: %d (expected: %d+-%d)\n", pulses, signature.avgPulses, tolerance);
    }

    return matches;
}

void ClickDetector::handleButtonPress(int pulses) {
    unsigned long now = millis();

    updateSignature(pulses);

    // FIXED: Block new clicks for 500ms after any callback to prevent RF echo
    if (now - lastCallbackTime < 500) {
        Serial.println("Ignoring - too soon after callback");
        return;
    }

    if (now - lastPress < debounceMs) {
        Serial.println("Debounced");
        return;
    }
    lastPress = now;

    clickCount++;

    if (clickCount == 1) {
        firstClickTime = now;
        Serial.println("First click (waiting for double/triple...)");
    }
    else if (clickCount == 2) {
        if (now - firstClickTime <= doubleClickMs) {
            secondClickTime = now;
            Serial.println("Second click (waiting for triple...)");
        } else {
            clickCount = 1;
            firstClickTime = now;
            Serial.println("First click (timeout - restarted)");
        }
    }
    else if (clickCount == 3) {
        if (now - secondClickTime <= tripleClickMs) {
            clickCount = 0;
            lastCallbackTime = now;  // FIXED: Mark callback time
            Serial.println("TRIPLE CLICK");
            if (tripleClickCallback) tripleClickCallback();
        } else {
            clickCount = 1;
            firstClickTime = now;
            Serial.println("First click (timeout - restarted)");
        }
    }
}

void ClickDetector::processSignal() {
    int pulses = readPulseCount();
    if (pulses < minPulses) return;

    if (!hasSignature) {
        updateSignature(pulses);
        if (signature.sampleCount >= 3) {
            Serial.printf("Button learned! Range: %d-%d pulses (avg: %d)\n",
                         signature.minPulses, signature.maxPulses, signature.avgPulses);
            Serial.println("Ready for single/double/triple click detection!");
        } else {
            Serial.printf("Learning... (%d/3 samples, %d pulses)\n", signature.sampleCount, pulses);
            Serial.println("   Press the SAME button again...");
        }
        return;
    }

    if (matchesSignature(pulses)) {
        Serial.printf("Button detected (%d pulses)!\n", pulses);
        handleButtonPress(pulses);
    } else {
        Serial.printf("Different button (%d pulses) - ignored\n", pulses);
    }
}

void ClickDetector::update() {
    unsigned long now = millis();

    // Check for timeout and trigger callbacks
    if (clickCount == 1 && (now - firstClickTime >= tripleClickMs)) {
        clickCount = 0;
        lastCallbackTime = now;  // FIXED: Mark callback time
        Serial.println("SINGLE CLICK");
        if (singleClickCallback) singleClickCallback();
    }
    else if (clickCount == 2 && (now - secondClickTime >= tripleClickMs)) {
        clickCount = 0;
        lastCallbackTime = now;  // FIXED: Mark callback time
        Serial.println("DOUBLE CLICK");
        if (doubleClickCallback) doubleClickCallback();
    }

    processSignal();
}

void ClickDetector::reset() {
    hasSignature = false;
    signature = {0, 0, 0, 0};
    clickCount = 0;
    lastCallbackTime = 0;
    Serial.println("ClickDetector reset");
}

bool ClickDetector::isLearned() {
    return hasSignature && signature.sampleCount >= 3;
}

void ClickDetector::getStatus(String& statusMsg) {
    if (isLearned()) {
        statusMsg = "Learned: " + String(signature.minPulses) + "-" +
                   String(signature.maxPulses) + " pulses (avg: " +
                   String(signature.avgPulses) + ")";
    } else {
        statusMsg = "Not learned yet (" + String(signature.sampleCount) + "/3 samples)";
    }
}

void ClickDetector::getBufferStats(String& stats) {
    RingbufHandle_t rb = nullptr;
    rmt_get_ringbuf_handle(rmtChannel, &rb);
    if (!rb) {
        stats = "No ring buffer";
        return;
    }

    UBaseType_t uxItemsWaiting = 0;
    vRingbufferGetInfo(rb, NULL, NULL, NULL, NULL, &uxItemsWaiting);

    stats = "Buffer items waiting: " + String(uxItemsWaiting);

    if (uxItemsWaiting > 10) {
        stats += " WARNING: High buffer usage";
    }
}

void ClickDetector::setDoubleClickTime(int ms) { doubleClickMs = ms; }
void ClickDetector::setTripleClickTime(int ms) { tripleClickMs = ms; }
void ClickDetector::setDebounceTime(int ms) { debounceMs = ms; }
void ClickDetector::setMinPulses(int min) { minPulses = min; }
void ClickDetector::setMaxPulses(int max) { maxPulses = max; }