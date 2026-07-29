#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#if defined(M5_ATOM_LED) || defined(M5_ATOMS3_LITE_LED)

#include <Arduino.h>

// LED states
enum LedState {
    WIFI_DISCONNECTED,
    WIFI_CONNECTED,
    RS485_TX,
    RS485_RX
};

class LedControl {
public:
    void begin();
    void setWifiConnected();
    void setWifiDisconnected();
    void flashTx();
    void flashRx();
    void update();

private:
    void showColor(uint32_t rgb); // 0xRRGGBB
    LedState currentState;
    unsigned long flashStartTime;
    bool isFlashing;
};

extern LedControl ledControl;

#endif // M5_ATOM_LED || M5_ATOMS3_LITE_LED

#endif
