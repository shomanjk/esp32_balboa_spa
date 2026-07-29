#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#ifdef M5_STATUS_LED

#include <Arduino.h>

// LED states
enum LedState {
    WIFI_DISCONNECTED,
    WIFI_CONNECTED,
    RS485_TX,
    RS485_RX,
    RS485_ALERT
};

/** Green/orange alternate severity while Wi‑Fi is up. */
enum class Rs485LedAlert : uint8_t {
    None = 0,      // solid green
    NoSpaData = 1, // slow — UART up, no valid spa frames
    SafeMode = 2,  // fast — UART skipped (wrong pins / fault streak)
};

class LedControl {
public:
    void begin();
    void setWifiConnected();
    void setWifiDisconnected();
    void setRs485LedAlert(Rs485LedAlert alert);
    void flashTx();
    void flashRx();
    void update();

private:
    void showColor(uint32_t rgb); // 0xRRGGBB
    LedState currentState;
    unsigned long flashStartTime;
    bool isFlashing;
    Rs485LedAlert rs485Alert;
    unsigned long alertToggleMs;
    bool alertShowOrange;
};

extern LedControl ledControl;

#endif // M5_STATUS_LED

#endif
