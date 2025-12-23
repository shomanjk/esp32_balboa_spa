#include "led_control.h"

LedControl ledControl;  // Global instance definition

void LedControl::begin() {
    M5.begin(true, false, true);  // Init LED, no serial, no I2C
    currentState = WIFI_DISCONNECTED;
    isFlashing = false;
    setWifiDisconnected();
}

void LedControl::setWifiConnected() {
    currentState = WIFI_CONNECTED;
    M5.dis.drawpix(0, 0x00ff00);  // Green
}

void LedControl::setWifiDisconnected() {
    currentState = WIFI_DISCONNECTED;
    M5.dis.drawpix(0, 0xff0000);  // Red
}

void LedControl::flashTx() {
    if (!isFlashing) {
        M5.dis.drawpix(0, 0x0000ff);  // Blue
        isFlashing = true;
        flashStartTime = millis();
    }
}

void LedControl::flashRx() {
    if (!isFlashing) {
        M5.dis.drawpix(0, 0xffff00);  // Yellow
        isFlashing = true;
        flashStartTime = millis();
    }
}

void LedControl::update() {
    if (isFlashing) {
        if (millis() - flashStartTime > 50) { // 50ms flash duration
            isFlashing = false;
            // Return to the previous state
            if (currentState == WIFI_CONNECTED) {
                setWifiConnected();
            } else {
                setWifiDisconnected();
            }
        }
    }
}