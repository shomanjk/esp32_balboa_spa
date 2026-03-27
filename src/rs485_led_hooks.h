#ifndef RS485_LED_HOOKS_H
#define RS485_LED_HOOKS_H

// Called from RS485 layer on real UART TX/RX (M5 Atom LED activity indicators).
void rs485LedNotifyTx(void);
void rs485LedNotifyRx(void);

#endif
