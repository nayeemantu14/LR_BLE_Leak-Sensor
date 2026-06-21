#include "app_leak_detection.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "app_conf.h"
#include "log_module.h"
#include "app_alert.h"
#include "ble_core.h"

#define ELEAK_FW_MAJOR  1
#define ELEAK_FW_MINOR  0
#define ELEAK_FW_PATCH  1

/* Debounce period in ms.  Must exceed the longest RF coupling glitch
 * (~1.5 ms from BLE TX at +10 dBm on the weak 390 KOhm pull-up). */
#define LEAK_DEBOUNCE_MS  50

/* ---------------------------------------------------------------------------
 * EXTI-based leak detection for PC13 (MSense).
 *
 * PC13 is a backup-domain pin on STM32WBA.  GPIO PUPDR pull-ups are silently
 * ignored by the hardware.  An external 390 KOhm pull-up on the PCB provides
 * the HIGH level when the sensor probes are dry.  When water bridges the
 * probes, the pin is pulled LOW.
 *
 * CRITICAL: CFG_BUTTON_SUPPORTED must be 0 in app_conf.h.
 * The BSP maps PC13 to Button 2 (B2_PIN = GPIO_PIN_13 in b_wba5m_wpan.h).
 * If enabled, the BSP hijacks EXTI13 and calls aci_gap_clear_security_db()
 * on every edge, completely preventing leak detection.
 *
 * EXTI13 priority is lowered from 0 (CubeMX default, same as radio) to 6
 * in the USER CODE MX_GPIO_Init_2 block of main.c.
 *
 * Detection flow:
 *   1. CubeMX configures PC13 as GPIO_MODE_IT_RISING_FALLING (both edges)
 *   2. USER CODE lowers EXTI13 priority to 6 (radio = 0, RCC = 1)
 *   3. EXTI13 fires on edge -> ISR disables EXTI13 NVIC, starts debounce timer
 *   4. After 50 ms the timer fires -> schedules sequencer task
 *   5. Task reads PC13 pin level: LOW = leak, HIGH = no leak
 *   6. If state changed: updates adv data, starts/stops alert
 *   7. Clears EXTI13 pending flags, re-enables EXTI13 NVIC
 *
 * Disabling EXTI13 during debounce prevents interrupt storms from RF coupling
 * (BLE TX at +10 dBm induces ~1.5 ms LOW glitches on the weak pull-up).
 * The 50 ms debounce comfortably exceeds the glitch duration.
 *
 * Power-up: A 500 ms beep indicates the device has powered on.
 * If the sensor probes are already wet at boot, leak state is set immediately.
 * --------------------------------------------------------------------------- */

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
static uint8_t leak_state;        /* 0 = no leak, 1 = leak */
static uint8_t a_peeraddr[8];
static volatile uint8_t init_done; /* gates EXTI callbacks until APP_LEAK_Init completes */

static void Leak_Process_Task(void);
static void LR_Switch_Task(void);
static void Leak_Debounce_Cb(void *arg);
static void LR_Debounce_Cb(void *arg);

void APP_LEAK_Init(void)
{
  tBleStatus status;

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
    LOG_INFO_APP(">> LEAK: Advertising started\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_enable FAILED 0x%02X\n", status);
  }

  /* Default state: no leak */
  leak_state = 0;
  a_EleakAdvData[18] = 0;

  /* Power-up indication beep — short single chirp via alert engine.
   * If a leak is detected immediately below, the leak pattern preempts. */
  APP_ALERT_PowerUpBeep();
  LOG_INFO_APP(">> LEAK: Power-up beep\n");

  /* Check initial pin state — detect leak already present at power-up.
   * PC13 is already configured as GPIO_MODE_IT_RISING_FALLING by CubeMX
   * MX_GPIO_Init.  EXTI13 priority was lowered to 6 in MX_GPIO_Init_2. */
  if (HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin) == GPIO_PIN_RESET)
  {
    leak_state = 1;
    a_EleakAdvData[18] = 1;
    aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                             sizeof(a_EleakAdvData), a_EleakAdvData);
    APP_ALERT_Start_Leak();
    LOG_INFO_APP(">> LEAK: LEAK DETECTED at power-up (PC13 LOW)\n");
  }

  /* Clear any pending EXTI flags accumulated during BLE init, then unblock
   * the EXTI callbacks.  CFG_BUTTON_SUPPORTED = 0 in app_conf.h prevents
   * the BSP from hijacking EXTI13 as Button 2 (B2_PIN = GPIO_PIN_13). */
  __HAL_GPIO_EXTI_CLEAR_IT(MSense_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(LR_BUT_Pin);
  NVIC_ClearPendingIRQ(EXTI13_IRQn);
  NVIC_ClearPendingIRQ(EXTI7_IRQn);
  init_done = 1;

  LOG_INFO_APP(">> LEAK: EXTI-based detection armed (PC13 = %s)\n",
               (HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin) == GPIO_PIN_RESET)
               ? "LOW/wet" : "HIGH/dry");
}

/**
 * @brief  EXTI callback for PC13 (MSense) — called from ISR context.
 *
 * Immediately disables EXTI13 NVIC to prevent interrupt storm from RF
 * coupling glitches, then starts a 50 ms debounce timer.  The timer callback
 * schedules a sequencer task that reads the settled pin level.
 */
void APP_LEAK_EXTI_Callback(void)
{
  /* Drop edges that arrive before APP_LEAK_Init has created the timers.
   * The HAL has already cleared the EXTI pending flag by the time we get
   * here, so no further cleanup is needed. */
  if (!init_done)
  {
    return;
  }

  /* Disable EXTI13 to prevent re-entry during debounce */
  HAL_NVIC_DisableIRQ(EXTI13_IRQn);

  /* Restart debounce timer (resets if already running) */
  UTIL_TIMER_Stop(&leak_debounce_timer);
  UTIL_TIMER_StartWithPeriod(&leak_debounce_timer, LEAK_DEBOUNCE_MS);
}

void APP_LEAK_LR_Callback(void)
{
  if (!init_done)
  {
    return;
  }

  UTIL_TIMER_Stop(&lr_debounce_timer);
  UTIL_TIMER_StartWithPeriod(&lr_debounce_timer, 50);
}

/**
 * @brief  Leak debounce timer callback — fires 50 ms after last EXTI edge.
 * @note   Timer ISR context — schedule sequencer task for main-loop processing.
 */
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
 * @brief  Leak process task — runs in main loop after debounce.
 *
 * Reads the actual PC13 pin level to determine leak state:
 *   LOW  = water bridging probes = leak detected
 *   HIGH = external pull-up = dry = no leak
 *
 * Only acts on state changes to avoid redundant BLE updates and alerts.
 * After processing, clears EXTI13 pending flags and re-enables the NVIC
 * so the next real edge is caught.
 */
static void Leak_Process_Task(void)
{
  GPIO_PinState pin = HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin);
  uint8_t new_state = (pin == GPIO_PIN_RESET) ? 1 : 0;

  if (new_state != leak_state)
  {
    leak_state = new_state;
    a_EleakAdvData[18] = leak_state;

    aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                             sizeof(a_EleakAdvData), a_EleakAdvData);

    if (leak_state)
    {
      APP_ALERT_Start_Leak();
      LOG_INFO_APP(">> LEAK: LEAK DETECTED (PC13 LOW after debounce)\n");
    }
    else
    {
      APP_ALERT_Stop();
      LOG_INFO_APP(">> LEAK: Leak cleared (PC13 HIGH after debounce)\n");
    }
  }

  /* Re-enable EXTI13: clear pending flags first to avoid immediate re-trigger
   * from edges that occurred during the debounce window. */
  __HAL_GPIO_EXTI_CLEAR_IT(MSense_Pin);
  NVIC_ClearPendingIRQ(EXTI13_IRQn);
  HAL_NVIC_EnableIRQ(EXTI13_IRQn);
}

static void LR_Switch_Task(void)
{
  LOG_INFO_APP(">> LEAK: LR switch toggled — resetting MCU\n");
  NVIC_SystemReset();
}

void APP_LEAK_Update_Battery(uint8_t batt_pct)
{
  a_EleakAdvData[19] = batt_pct;
  aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                           sizeof(a_EleakAdvData), a_EleakAdvData);
}
