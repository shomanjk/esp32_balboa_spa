#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <M5Atom.h>

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
    LedState currentState;
    unsigned long flashStartTime;
    bool isFlashing;
};

extern LedControl ledControl;

#endif