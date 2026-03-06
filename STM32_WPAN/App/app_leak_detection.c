#include "app_leak_detection.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "app_conf.h"
#include "log_module.h"
#include "app_alert.h"
#include "ble_core.h"

static uint8_t a_EleakAdvData[20] = {
  0x02, 0x0A, 0x1F,                              /* TX Power Level (+10 dBm) */
  0x06, 0x09, 'e', 'l', 'e', 'a', 'k',          /* Complete Local Name "eleak" */
  0x03, 0x19, 0x01, 0x80,                         /* Appearance: Generic Sensor */
  0x05, 0xFF, 0x30, 0x00, 0x00, 0x64             /* Mfg: ST, leak=0, batt=100% */
};

static Adv_Set_t eleak_adv_set;
static UTIL_TIMER_Object_t leak_debounce_timer;
static UTIL_TIMER_Object_t lr_debounce_timer;
static uint8_t leak_state;
static uint8_t a_peeraddr[8];

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
    LOG_INFO_APP(">> LEAK: Extended advertising started\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_enable FAILED 0x%02X\n", status);
  }

  /* Read initial leak sensor state */
  leak_state = (HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin) == GPIO_PIN_RESET) ? 1 : 0;
  a_EleakAdvData[18] = leak_state;

  if (leak_state)
  {
    /* Update adv data with initial leak state */
    aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                             sizeof(a_EleakAdvData), a_EleakAdvData);
    APP_ALERT_Start_Leak();
    LOG_INFO_APP(">> LEAK: Initial state = LEAK DETECTED\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: Initial state = No leak\n");
  }
}

void APP_LEAK_EXTI_Callback(void)
{
  UTIL_TIMER_Stop(&leak_debounce_timer);
  UTIL_TIMER_StartWithPeriod(&leak_debounce_timer, 50);
}

void APP_LEAK_LR_Callback(void)
{
  UTIL_TIMER_Stop(&lr_debounce_timer);
  UTIL_TIMER_StartWithPeriod(&lr_debounce_timer, 50);
}

static void Leak_Debounce_Cb(void *arg)
{
  UTIL_SEQ_SetTask(1U << CFG_TASK_LEAK_PROCESS, CFG_SEQ_PRIO_0);
}

static void LR_Debounce_Cb(void *arg)
{
  UTIL_SEQ_SetTask(1U << CFG_TASK_LR_SWITCH, CFG_SEQ_PRIO_0);
}

static void Leak_Process_Task(void)
{
  uint8_t new_state = (HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin) == GPIO_PIN_RESET) ? 1 : 0;

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
