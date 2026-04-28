#ifndef APP_ALERT_H
#define APP_ALERT_H

void APP_ALERT_Init(void);
void APP_ALERT_Start_Leak(void);
void APP_ALERT_Stop(void);
void APP_ALERT_Start_LowBatt(void);
void APP_ALERT_Stop_LowBatt(void);

/* Block / release Stop1 from the buzzer LPM client.  Use these around any
 * direct buzzer drive outside the alert state machine (e.g. power-up beep). */
void APP_ALERT_BlockLPM(void);
void APP_ALERT_ReleaseLPM(void);

#endif /* APP_ALERT_H */
