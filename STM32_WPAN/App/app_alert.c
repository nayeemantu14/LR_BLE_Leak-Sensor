#include "app_alert.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "app_conf.h"
#include "log_module.h"

static UTIL_TIMER_Object_t alert_period_timer;
static UTIL_TIMER_Object_t alert_on_timer;
static uint8_t alert_active;

static void Alert_Tick_Task(void);
static void Alert_Off_Task(void);
static void Alert_Period_Cb(void *arg);
static void Alert_On_Cb(void *arg);

void APP_ALERT_Init(void)
{
  UTIL_SEQ_RegTask(1U << CFG_TASK_ALERT_TICK, UTIL_SEQ_RFU, Alert_Tick_Task);
  UTIL_SEQ_RegTask(1U << CFG_TASK_ALERT_OFF, UTIL_SEQ_RFU, Alert_Off_Task);
  UTIL_TIMER_Create(&alert_period_timer, 0, UTIL_TIMER_PERIODIC, Alert_Period_Cb, NULL);
  UTIL_TIMER_Create(&alert_on_timer, 0, UTIL_TIMER_ONESHOT, Alert_On_Cb, NULL);
  alert_active = 0;
}

void APP_ALERT_Start_Leak(void)
{
  if (alert_active)
  {
    return;
  }
  alert_active = 1;
  LOG_INFO_APP(">> ALERT: Leak alert started\n");
  UTIL_SEQ_SetTask(1U << CFG_TASK_ALERT_TICK, CFG_SEQ_PRIO_1);
  UTIL_TIMER_StartWithPeriod(&alert_period_timer, 5000);
}

void APP_ALERT_Stop(void)
{
  UTIL_TIMER_Stop(&alert_period_timer);
  UTIL_TIMER_Stop(&alert_on_timer);
  HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_RESET);
  alert_active = 0;
  LOG_INFO_APP(">> ALERT: Leak alert stopped\n");
}

static void Alert_Period_Cb(void *arg)
{
  UTIL_SEQ_SetTask(1U << CFG_TASK_ALERT_TICK, CFG_SEQ_PRIO_1);
}

static void Alert_Tick_Task(void)
{
  HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_SET);
  UTIL_TIMER_StartWithPeriod(&alert_on_timer, 500);
}

static void Alert_On_Cb(void *arg)
{
  UTIL_SEQ_SetTask(1U << CFG_TASK_ALERT_OFF, CFG_SEQ_PRIO_1);
}

static void Alert_Off_Task(void)
{
  HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_RESET);
}
