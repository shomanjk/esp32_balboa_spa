#ifdef M5_STATUS_LED

#include "led_control.h"

#include <FastLED.h>

#ifndef M5_STATUS_LED_PIN
#error "M5_STATUS_LED requires -DM5_STATUS_LED_PIN=<gpio> (Atom Lite 27, AtomS3 Lite 35)"
#endif
#ifndef M5_STATUS_LED_COUNT
#define M5_STATUS_LED_COUNT 1
#endif

static CRGB statusLeds[M5_STATUS_LED_COUNT];

LedControl ledControl;

void LedControl::showColor(uint32_t rgb) {
    statusLeds[0] = CRGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    FastLED.show();
}

void LedControl::begin() {
    FastLED.addLeds<WS2812, M5_STATUS_LED_PIN, GRB>(statusLeds, M5_STATUS_LED_COUNT);
    FastLED.setBrightness(40);
    currentState = WIFI_DISCONNECTED;
    isFlashing = false;
    setWifiDisconnected();
}

void LedControl::setWifiConnected() {
    currentState = WIFI_CONNECTED;
    showColor(0x00ff00);  // Green
}

void LedControl::setWifiDisconnected() {
    currentState = WIFI_DISCONNECTED;
    showColor(0xff0000);  // Red
}

void LedControl::flashTx() {
    if (!isFlashing) {
        showColor(0x0000ff);  // Blue
        isFlashing = true;
        flashStartTime = millis();
    }
}

void LedControl::flashRx() {
    if (!isFlashing) {
        showColor(0xffff00);  // Yellow
        isFlashing = true;
        flashStartTime = millis();
    }
}

void LedControl::update() {
    if (isFlashing) {
        if (millis() - flashStartTime > 50) { // 50ms flash duration
            isFlashing = false;
            if (currentState == WIFI_CONNECTED) {
                setWifiConnected();
            } else {
                setWifiDisconnected();
            }
        }
    }
}

#endif // M5_STATUS_LED
