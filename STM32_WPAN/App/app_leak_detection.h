#ifndef APP_LEAK_DETECTION_H
#define APP_LEAK_DETECTION_H

#include <stdint.h>

void APP_LEAK_Init(void);
void APP_LEAK_EXTI_Callback(void);
void APP_LEAK_LR_Callback(void);
void APP_LEAK_Update_Battery(uint8_t batt_pct);

#endif /* APP_LEAK_DETECTION_H */
