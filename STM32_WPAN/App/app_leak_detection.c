#include "app_leak_detection.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "app_conf.h"
#include "log_module.h"
#include "app_alert.h"
#include "ble_core.h"

#define ELEAK_FW_MAJOR  0
#define ELEAK_FW_MINOR  1
#define ELEAK_FW_PATCH  0

/* ---------------------------------------------------------------------------
 * EXTI-based leak detection for PC13 (backup-domain pin) with storm prevention.
 *
 * PC13 on STM32WBA is TAMP1/WKUP4 — a backup-domain pin that does NOT
 * support internal pull-up/pull-down.  The external pull-up R9 (390 K)
 * is marginal: BLE radio RF coupling at +10 dBm can cause brief LOW
 * glitches during TX events.
 *
 * Three fixes applied vs. naive EXTI debounce:
 *
 * 1. EXTI13 priority lowered to 6 (below RADIO_INTR_PRIO_HIGH = 0).
 *    CubeMX sets it to 0, same as the active radio ISR.  Since EXTI13
 *    has a lower IRQ number than RADIO_IRQn, it wins arbitration and
 *    preempts radio operations — disrupting BLE timing during the very
 *    TX events that cause RF coupling.
 *
 * 2. EXTI13 is disabled inside the callback and only re-enabled after
 *    the debounce task completes.  This prevents the "EXTI storm" where
 *    each advertising TX (~every 375 ms) generates rising + falling edges
 *    that continuously restart the debounce timer.
 *
 * 3. Debounce period = 500 ms (not 200 ms).  This ensures the pin read
 *    lands between advertising events regardless of interval jitter.
 *
 * Hardware fix for next board rev: change R9 from 390 K to 47 K.
 * --------------------------------------------------------------------------- */

#define LEAK_DEBOUNCE_MS    500U   /* Debounce delay (ms) — must exceed adv interval */

static uint8_t a_EleakAdvData[23] = {
  0x02, 0x0A, 0x1F,                              /* TX Power Level (+10 dBm) */
  0x06, 0x09, 'e', 'l', 'e', 'a', 'k',          /* Complete Local Name "eleak" */
  0x03, 0x19, 0x01, 0x80,                         /* Appearance: Generic Sensor */
  0x08, 0xFF, 0x30, 0x00, 0x00, 0x64,            /* Mfg: ST, leak=0, batt=100% */
  ELEAK_FW_MAJOR, ELEAK_FW_MINOR, ELEAK_FW_PATCH /* FW version 0.1.0 */
};

static Adv_Set_t eleak_adv_set;
static UTIL_TIMER_Object_t leak_debounce_timer;
static UTIL_TIMER_Object_t lr_debounce_timer;
static uint8_t leak_state;
static volatile uint8_t exti_debouncing;  /* 1 = debounce in progress, ignore edges */
static uint8_t a_peeraddr[8];

static void Leak_Process_Task(void);
static void LR_Switch_Task(void);
static void Leak_Debounce_Cb(void *arg);
static void LR_Debounce_Cb(void *arg);

void APP_LEAK_Init(void)
{
  tBleStatus status;

  /*
   * Disable MSense and LR_BUT EXTIs during init.
   * MX_GPIO_Init() enables these at priority 0 before timers/tasks exist.
   */
  HAL_NVIC_DisableIRQ(EXTI13_IRQn);
  HAL_NVIC_DisableIRQ(EXTI7_IRQn);

  /* Register sequencer tasks */
  UTIL_SEQ_RegTask(1U << CFG_TASK_LEAK_PROCESS, UTIL_SEQ_RFU, Leak_Process_Task);
  UTIL_SEQ_RegTask(1U << CFG_TASK_LR_SWITCH, UTIL_SEQ_RFU, LR_Switch_Task);

  /* Create debounce timers */
  UTIL_TIMER_Create(&leak_debounce_timer, 0, UTIL_TIMER_ONESHOT, Leak_Debounce_Cb, NULL);
  UTIL_TIMER_Create(&lr_debounce_timer, 0, UTIL_TIMER_ONESHOT, LR_Debounce_Cb, NULL);

  /* Read LR_BUT pin to select PHY: GND = Coded PHY (Long Range), HIGH = 1M PHY */
  uint8_t lr_mode = (HAL_GPIO_ReadPin(LR_BUT_GPIO_Port, LR_BUT_Pin) == GPIO_PIN_RESET) ? 1 : 0;

  /*
   * 1M mode:    Legacy advertising on primary channels only (phones can see it)
   *             ADV_SCAN_IND = non-connectable, scannable
   * Coded mode: Extended advertising on secondary channels (long range)
   *             Non-connectable extended advertising with Coded PHY S=8
   */
  uint8_t adv_mode       = lr_mode ? 0x02 : 0x00;
  uint8_t secondary_phy  = lr_mode ? 0x03 : 0x01;
  uint16_t adv_properties = lr_mode
    ? 0                                                                 /* Extended: non-connectable, non-scannable */
    : (HCI_ADV_EVENT_PROP_LEGACY | HCI_ADV_EVENT_PROP_SCANNABLE);      /* Legacy ADV_SCAN_IND: primary channels only */

  LOG_INFO_APP(">> LEAK: PHY = %s\n", lr_mode ? "Coded (Long Range)" : "1M (Legacy)");

  /* Configure advertising with selected PHY and mode */
  status = aci_gap_adv_set_configuration(
    adv_mode,                           /* Adv_Mode: Coded or 1M primary PHY */
    0,                                  /* Advertising_Handle */
    adv_properties,                     /* Legacy (1M) or Extended (Coded) */
    500, 700,                           /* Interval: 312.5 - 437.5 ms */
    ADV_CH_37 | ADV_CH_38 | ADV_CH_39, /* All channels */
    GAP_PUBLIC_ADDR,                    /* Own_Address_Type */
    GAP_PUBLIC_ADDR,                    /* Peer_Address_Type */
    a_peeraddr,                         /* Peer_Address */
    HCI_ADV_FILTER_NO,                  /* Filter_Policy */
    0x1F,                               /* TX Power: +10 dBm */
    0x00,                               /* Secondary_Adv_Max_Skip */
    secondary_phy,                      /* Secondary_Adv_PHY */
    0x00,                               /* SID */
    0x00);                              /* Scan_Req_Notification_Enable */

  if (status == BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_configuration OK\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_configuration FAILED 0x%02X\n", status);
  }

  /* Set advertising data */
  status = aci_gap_adv_set_adv_data(
    0,                                  /* Advertising_Handle */
    HCI_SET_ADV_DATA_OPERATION_COMPLETE,
    0,                                  /* Fragment_Preference */
    sizeof(a_EleakAdvData),
    a_EleakAdvData);

  if (status == BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_adv_data OK\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_adv_data FAILED 0x%02X\n", status);
  }

  /* Enable advertising (continuous) */
  eleak_adv_set.Advertising_Handle = 0;
  eleak_adv_set.Duration = 0;
  eleak_adv_set.Max_Extended_Advertising_Events = 0;

  status = aci_gap_adv_set_enable(1, 1, &eleak_adv_set);

  if (status == BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP(">> LEAK: Extended advertising started\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_enable FAILED 0x%02X\n", status);
  }

  /* Default state: no leak.  Do NOT read the pin here — radio is active
   * and the weak 390 K pull-up can't guarantee a clean read.
   * The first EXTI edge will trigger the debounce → read cycle. */
  leak_state = 0;
  exti_debouncing = 0;
  a_EleakAdvData[18] = 0;

  LOG_INFO_APP(">> LEAK: Initial state = No leak (EXTI debounce = %u ms)\n",
               LEAK_DEBOUNCE_MS);

  /* Clear any pending flags that accumulated during init */
  __HAL_GPIO_EXTI_CLEAR_IT(MSense_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(LR_BUT_Pin);
  NVIC_ClearPendingIRQ(EXTI13_IRQn);
  NVIC_ClearPendingIRQ(EXTI7_IRQn);

  /*
   * FIX: Lower EXTI13 priority from 0 to 6.
   * CubeMX sets it to 0 — same as RADIO_INTR_PRIO_HIGH.  Since EXTI13
   * has a lower IRQ number than RADIO_IRQn, it wins arbitration and
   * preempts the radio during TX events.  This disrupts BLE timing and
   * causes a positive feedback loop: disrupted radio → retries → more
   * RF coupling → more EXTI edges → more disruption.
   * Priority 6 is safely below radio (0) and RCC (1).
   */
  HAL_NVIC_SetPriority(EXTI13_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI13_IRQn);

  HAL_NVIC_SetPriority(EXTI7_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI7_IRQn);
}

/**
 * @brief  MSense EXTI callback — debounce with storm prevention.
 *
 * Called on both rising and falling edges of PC13 (MSense).
 * Disables EXTI13 immediately to prevent RF-coupled edge storm, then
 * starts a 500 ms one-shot debounce timer.  EXTI13 is re-enabled only
 * after Leak_Process_Task reads the pin and acts on the result.
 */
void APP_LEAK_EXTI_Callback(void)
{
  if (exti_debouncing)
  {
    return;  /* Already debouncing — ignore this edge */
  }
  exti_debouncing = 1;

  /* Disable EXTI13 to stop the edge storm.
   * RF coupling on the weak 390 K pull-up generates edges on every
   * advertising TX event.  Without this, the debounce timer gets
   * continuously restarted and never completes. */
  HAL_NVIC_DisableIRQ(EXTI13_IRQn);

  UTIL_TIMER_StartWithPeriod(&leak_debounce_timer, LEAK_DEBOUNCE_MS);
}

void APP_LEAK_LR_Callback(void)
{
  UTIL_TIMER_Stop(&lr_debounce_timer);
  UTIL_TIMER_StartWithPeriod(&lr_debounce_timer, 50);
}

static void Leak_Debounce_Cb(void *arg)
{
  (void)arg;
  UTIL_SEQ_SetTask(1U << CFG_TASK_LEAK_PROCESS, CFG_SEQ_PRIO_0);
}

static void LR_Debounce_Cb(void *arg)
{
  (void)arg;
  UTIL_SEQ_SetTask(1U << CFG_TASK_LR_SWITCH, CFG_SEQ_PRIO_0);
}

/**
 * @brief  Leak process task — reads pin after debounce, re-enables EXTI.
 *
 * Called 500 ms after the EXTI edge.  EXTI13 is disabled during this
 * entire window, so no new edges can interfere.  The 500 ms delay
 * guarantees the read lands in a quiet gap between advertising events
 * (interval 312.5–437.5 ms, TX burst ~1.5 ms).
 */
static void Leak_Process_Task(void)
{
  GPIO_PinState pin = HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin);
  uint8_t new_state = (pin == GPIO_PIN_RESET) ? 1 : 0;

  LOG_INFO_APP(">> LEAK: Pin read = %s (current state = %s)\n",
               new_state ? "LOW (wet)" : "HIGH (dry)",
               leak_state ? "LEAK" : "No leak");

  if (new_state != leak_state)
  {
    leak_state = new_state;
    a_EleakAdvData[18] = leak_state;

    aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                             sizeof(a_EleakAdvData), a_EleakAdvData);

    if (leak_state)
    {
      APP_ALERT_Start_Leak();
      LOG_INFO_APP(">> LEAK: LEAK DETECTED\n");
    }
    else
    {
      APP_ALERT_Stop();
      LOG_INFO_APP(">> LEAK: Leak cleared\n");
    }
  }

  /* Re-enable EXTI13 to detect next transition.
   * Clear both the EXTI peripheral flag and NVIC pending bit to
   * prevent an immediate re-trigger from stale edges. */
  __HAL_GPIO_EXTI_CLEAR_IT(MSense_Pin);
  NVIC_ClearPendingIRQ(EXTI13_IRQn);
  HAL_NVIC_EnableIRQ(EXTI13_IRQn);
  exti_debouncing = 0;
}

static void LR_Switch_Task(void)
{
  LOG_INFO_APP(">> LEAK: LR switch changed — resetting MCU\n");
  NVIC_SystemReset();
}

void APP_LEAK_Update_Battery(uint8_t batt_pct)
{
  a_EleakAdvData[19] = batt_pct;
  aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                           sizeof(a_EleakAdvData), a_EleakAdvData);
}
