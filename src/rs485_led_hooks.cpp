#include "rs485_led_hooks.h"

#ifdef M5_ATOM_LED
#include "led_control.h"

void rs485LedNotifyTx(void)
{
  ledControl.flashTx();
}

void rs485LedNotifyRx(void)
{
  ledControl.flashRx();
}
#else
void rs485LedNotifyTx(void) {}
void rs485LedNotifyRx(void) {}
#endif
