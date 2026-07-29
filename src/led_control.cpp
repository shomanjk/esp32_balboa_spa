#ifdef M5_STATUS_LED

#include "led_control.h"

#include <FastLED.h>

#ifndef M5_STATUS_LED_PIN
#error "M5_STATUS_LED requires -DM5_STATUS_LED_PIN=<gpio> (Atom Lite 27, AtomS3 Lite 35)"
#endif
#ifndef M5_STATUS_LED_COUNT
#define M5_STATUS_LED_COUNT 1
#endif

// Green <-> orange blink periods (ms half-cycle).
static const unsigned long RS485_LED_SAFE_MODE_MS = 200;  // fast
static const unsigned long RS485_LED_NO_DATA_MS = 1250;    // slow

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
    rs485Alert = Rs485LedAlert::None;
    alertToggleMs = 0;
    alertShowOrange = false;
    setWifiDisconnected();
}

void LedControl::setWifiConnected() {
    currentState = WIFI_CONNECTED;
    if (rs485Alert == Rs485LedAlert::None && !isFlashing)
    {
        showColor(0x00ff00);  // Green
    }
}

void LedControl::setWifiDisconnected() {
    currentState = WIFI_DISCONNECTED;
    rs485Alert = Rs485LedAlert::None;
    if (!isFlashing)
    {
        showColor(0xff0000);  // Red
    }
}

void LedControl::setRs485LedAlert(Rs485LedAlert alert) {
    if (rs485Alert == alert)
    {
        return;
    }
    rs485Alert = alert;
    if (alert != Rs485LedAlert::None)
    {
        currentState = RS485_ALERT;
        alertToggleMs = millis();
        alertShowOrange = false;
        showColor(0x00ff00); // start on green (Wi-Fi up)
    }
    else if (currentState == RS485_ALERT || currentState == WIFI_CONNECTED)
    {
        currentState = WIFI_CONNECTED;
        if (!isFlashing)
        {
            showColor(0x00ff00);
        }
    }
}

void LedControl::flashTx() {
    if (rs485Alert != Rs485LedAlert::None)
    {
        return;
    }
    if (!isFlashing) {
        showColor(0x0000ff);  // Blue
        isFlashing = true;
        flashStartTime = millis();
    }
}

void LedControl::flashRx() {
    if (rs485Alert != Rs485LedAlert::None)
    {
        return;
    }
    if (!isFlashing) {
        showColor(0xffff00);  // Yellow
        isFlashing = true;
        flashStartTime = millis();
    }
}

void LedControl::update() {
    if (rs485Alert != Rs485LedAlert::None)
    {
        const unsigned long periodMs =
            (rs485Alert == Rs485LedAlert::SafeMode) ? RS485_LED_SAFE_MODE_MS : RS485_LED_NO_DATA_MS;
        if (millis() - alertToggleMs >= periodMs)
        {
            alertToggleMs = millis();
            alertShowOrange = !alertShowOrange;
            showColor(alertShowOrange ? 0xff8000u : 0x00ff00u); // orange <-> green
        }
        return;
    }
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
