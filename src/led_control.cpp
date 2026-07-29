#if defined(M5_ATOM_LED) || defined(M5_ATOMS3_LITE_LED)

#include "led_control.h"

#ifdef M5_ATOM_LED
#include <M5Atom.h>
#endif

#ifdef M5_ATOMS3_LITE_LED
#include <FastLED.h>
// AtomS3 Lite onboard WS2812C (SKU C124) — not M5Unified M5.Led.
#ifndef M5_ATOMS3_LITE_LED_PIN
#define M5_ATOMS3_LITE_LED_PIN 35
#endif
#ifndef M5_ATOMS3_LITE_NUM_LEDS
#define M5_ATOMS3_LITE_NUM_LEDS 1
#endif
static CRGB atoms3LiteLeds[M5_ATOMS3_LITE_NUM_LEDS];
#endif

LedControl ledControl;

void LedControl::showColor(uint32_t rgb) {
#ifdef M5_ATOM_LED
    // Pass-through: same packed values as the historical Atom Lite led_control.cpp
    // (M5.dis.drawpix color packing). Do not reinterpret bytes here.
    M5.dis.drawpix(0, rgb);
#endif
#ifdef M5_ATOMS3_LITE_LED
    atoms3LiteLeds[0] = CRGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    FastLED.show();
#endif
}

void LedControl::begin() {
#ifdef M5_ATOM_LED
    M5.begin(true, false, true);  // Init LED, no serial, no I2C
#endif
#ifdef M5_ATOMS3_LITE_LED
    FastLED.addLeds<WS2812, M5_ATOMS3_LITE_LED_PIN, GRB>(atoms3LiteLeds, M5_ATOMS3_LITE_NUM_LEDS);
    FastLED.setBrightness(40);
#endif
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

#endif // M5_ATOM_LED || M5_ATOMS3_LITE_LED
