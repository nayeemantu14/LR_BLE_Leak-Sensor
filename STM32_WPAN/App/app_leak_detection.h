#ifndef APP_LEAK_DETECTION_H
#define APP_LEAK_DETECTION_H

#include <stdint.h>

void APP_LEAK_Init(void);
void APP_LEAK_EXTI_Callback(void);
void APP_LEAK_LR_Callback(void);
void APP_LEAK_Update_Battery(uint8_t batt_pct);

/* HCI_LE_ADVERTISING_SET_TERMINATED hook — called from the BLE event path
 * (app_ble.c, USER CODE SUBEVENT) when a bounded advertising burst expires. */
void APP_LEAK_OnAdvTerminated(uint8_t adv_handle, uint8_t status);

#endif /* APP_LEAK_DETECTION_H */
