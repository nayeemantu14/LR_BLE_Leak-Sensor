#include "app_alert.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "stm32_lpm.h"
#include "stm32_lpm_if.h"
#include "app_conf.h"
#include "log_module.h"

/* Block Stop1 while the buzzer is ON.  In Stop1 the high-frequency clocks
 * are gated and although GPIO output state is retained, on this hardware
 * the buzzer fails to drive audibly during Stop1 entry/exit transients.
 * Holding UTIL_LPM_SLEEP_MODE keeps the chip in Sleep (CPU off, peripherals
 * running) for the few-hundred-ms beep window — clean drive, low cost. */
#define BUZZER_LPM_BLOCK()    UTIL_LPM_SetMaxMode(1U << CFG_LPM_BUZZER, UTIL_LPM_SLEEP_MODE)
#define BUZZER_LPM_RELEASE()  UTIL_LPM_SetMaxMode(1U << CFG_LPM_BUZZER, UTIL_LPM_MAX_MODE)

void APP_ALERT_BlockLPM(void)   { BUZZER_LPM_BLOCK(); }
void APP_ALERT_ReleaseLPM(void) { BUZZER_LPM_RELEASE(); }

/* --- Leak alert: single 500ms sustained beep every 5s --- */
static UTIL_TIMER_Object_t alert_period_timer;
static UTIL_TIMER_Object_t alert_on_timer;
static uint8_t alert_active;

static void Alert_Tick_Task(void);
static void Alert_Off_Task(void);
static void Alert_Period_Cb(void *arg);
static void Alert_On_Cb(void *arg);

/* --- Low battery alert: double 100ms beep every 10s --- */
static UTIL_TIMER_Object_t lowbatt_period_timer;
static UTIL_TIMER_Object_t lowbatt_beep_timer;
static uint8_t lowbatt_active;
static uint8_t lowbatt_beep_state;

static void LowBatt_Beep_Task(void);
static void LowBatt_Period_Cb(void *arg);
static void LowBatt_Beep_Cb(void *arg);

/* ========== Initialization ========== */

void APP_ALERT_Init(void)
{
  /* Leak alert tasks and timers */
  UTIL_SEQ_RegTask(1U << CFG_TASK_ALERT_TICK, UTIL_SEQ_RFU, Alert_Tick_Task);
  UTIL_SEQ_RegTask(1U << CFG_TASK_ALERT_OFF, UTIL_SEQ_RFU, Alert_Off_Task);
  UTIL_TIMER_Create(&alert_period_timer, 0, UTIL_TIMER_PERIODIC, Alert_Period_Cb, NULL);
  UTIL_TIMER_Create(&alert_on_timer, 0, UTIL_TIMER_ONESHOT, Alert_On_Cb, NULL);
  alert_active = 0;

  /* Low battery alert task and timers */
  UTIL_SEQ_RegTask(1U << CFG_TASK_LOWBATT_BEEP, UTIL_SEQ_RFU, LowBatt_Beep_Task);
  UTIL_TIMER_Create(&lowbatt_period_timer, 0, UTIL_TIMER_PERIODIC, LowBatt_Period_Cb, NULL);
  UTIL_TIMER_Create(&lowbatt_beep_timer, 0, UTIL_TIMER_ONESHOT, LowBatt_Beep_Cb, NULL);
  lowbatt_active = 0;
  lowbatt_beep_state = 0;
}

/* ========== Leak Alert (500ms sustained beep every 5s) ========== */

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
  /* Always release the LPM hold here in case the alert was cancelled mid-beep
   * (between Alert_Tick_Task and Alert_Off_Task). */
  BUZZER_LPM_RELEASE();
  LOG_INFO_APP(">> ALERT: Leak alert stopped\n");
}

static void Alert_Period_Cb(void *arg)
{
  UTIL_SEQ_SetTask(1U << CFG_TASK_ALERT_TICK, CFG_SEQ_PRIO_1);
}

static void Alert_Tick_Task(void)
{
  /* Hold the chip out of Stop1 while the buzzer is driven, so the audio
   * output is clean.  Released by Alert_Off_Task after 500 ms. */
  BUZZER_LPM_BLOCK();
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
  /* Buzzer is off — release the LPM hold so the chip can return to Stop1. */
  BUZZER_LPM_RELEASE();
}

/* ========== Low Battery Alert (double 100ms beep every 10s) ========== */

void APP_ALERT_Start_LowBatt(void)
{
  if (lowbatt_active)
  {
    return;
  }
  lowbatt_active = 1;
  LOG_INFO_APP(">> ALERT: Low battery alert started\n");

  /* Trigger immediate first double-beep, then repeat every 10s */
  lowbatt_beep_state = 0;
  UTIL_SEQ_SetTask(1U << CFG_TASK_LOWBATT_BEEP, CFG_SEQ_PRIO_1);
  UTIL_TIMER_StartWithPeriod(&lowbatt_period_timer, 10000);
}

void APP_ALERT_Stop_LowBatt(void)
{
  if (!lowbatt_active)
  {
    return;
  }
  UTIL_TIMER_Stop(&lowbatt_period_timer);
  UTIL_TIMER_Stop(&lowbatt_beep_timer);
  HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_RESET);
  lowbatt_active = 0;
  lowbatt_beep_state = 0;
  /* Release LPM hold in case the low-batt sequence was cancelled mid-beep */
  BUZZER_LPM_RELEASE();
  LOG_INFO_APP(">> ALERT: Low battery alert stopped\n");
}

static void LowBatt_Period_Cb(void *arg)
{
  lowbatt_beep_state = 0;
  UTIL_SEQ_SetTask(1U << CFG_TASK_LOWBATT_BEEP, CFG_SEQ_PRIO_1);
}

static void LowBatt_Beep_Cb(void *arg)
{
  UTIL_SEQ_SetTask(1U << CFG_TASK_LOWBATT_BEEP, CFG_SEQ_PRIO_1);
}

/**
 * @brief  Double-beep state machine: 100ms ON → 100ms OFF → 100ms ON → OFF
 * @note   Produces two short pulses, distinct from leak alert's single 500ms beep.
 *         If a leak alert is active, the low-batt beep is suppressed (leak takes priority).
 */
static void LowBatt_Beep_Task(void)
{
  /* Suppress low-batt beep while leak alert is active (leak is higher priority) */
  if (alert_active)
  {
    lowbatt_beep_state = 0;
    return;
  }

  switch (lowbatt_beep_state)
  {
    case 0: /* First pulse ON — block LPM for the full double-beep sequence */
      BUZZER_LPM_BLOCK();
      HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_SET);
      lowbatt_beep_state = 1;
      UTIL_TIMER_StartWithPeriod(&lowbatt_beep_timer, 100);
      break;

    case 1: /* First pulse OFF */
      HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_RESET);
      HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_RESET);
      lowbatt_beep_state = 2;
      UTIL_TIMER_StartWithPeriod(&lowbatt_beep_timer, 100);
      break;

    case 2: /* Second pulse ON */
      HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_SET);
      lowbatt_beep_state = 3;
      UTIL_TIMER_StartWithPeriod(&lowbatt_beep_timer, 100);
      break;

    case 3: /* Second pulse OFF — sequence done */
      HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, GPIO_PIN_RESET);
      HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, GPIO_PIN_RESET);
      lowbatt_beep_state = 0;
      /* Sequence complete — release LPM hold */
      BUZZER_LPM_RELEASE();
      break;

    default:
      lowbatt_beep_state = 0;
      break;
  }
}
