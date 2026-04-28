#include "app_battery.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "app_conf.h"
#include "log_module.h"
#include "adc_ctrl.h"
#include "app_leak_detection.h"
#include "app_alert.h"

/* Adaptive battery read intervals (ms) */
#define BATT_PERIOD_NORMAL_MS    3600000U  /* 1 hour — battery >= 20% */
#define BATT_PERIOD_LOW_MS         60000U  /* 1 minute — battery 10-19% */
#define BATT_PERIOD_CRITICAL_MS    10000U  /* 10 seconds — battery < 10% */

/* ADC handle for battery voltage reading — PA6 = ADC4_IN3 */
static ADCCTRL_Handle_t BatteryRequest_Handle =
{
  .Uid = 0x00,
  .State = ADCCTRL_HANDLE_NOT_REG,
  .InitConf =
  {
    .ConvParams =
    {
      .TriggerFrequencyMode = LL_ADC_TRIGGER_FREQ_LOW,
      .Resolution = LL_ADC_RESOLUTION_12B,
      .DataAlign = LL_ADC_DATA_ALIGN_RIGHT,
      .TriggerStart = LL_ADC_REG_TRIG_SOFTWARE,
      .TriggerEdge = LL_ADC_REG_TRIG_EXT_RISING,
      .ConversionMode = LL_ADC_REG_CONV_SINGLE,
      .DmaTransfer = LL_ADC_REG_DMA_TRANSFER_NONE,
      .Overrun = LL_ADC_REG_OVR_DATA_OVERWRITTEN,
      .SamplingTimeCommon1 = LL_ADC_SAMPLINGTIME_814CYCLES_5,
      .SamplingTimeCommon2 = LL_ADC_SAMPLINGTIME_1CYCLE_5
    },
    .SeqParams =
    {
      .Setup = LL_ADC_REG_SEQ_CONFIGURABLE,
      .Length = LL_ADC_REG_SEQ_SCAN_DISABLE,
      .DiscMode = LL_ADC_REG_SEQ_DISCONT_DISABLE
    },
    .LowPowerParams =
    {
      .AutoPowerOff = DISABLE,
      .AutonomousDPD = LL_ADC_LP_AUTONOMOUS_DPD_DISABLE
    }
  },
  .ChannelConf =
  {
    .Channel = LL_ADC_CHANNEL_3,    /* IN3 on PA6 — battery voltage */
    .Rank = LL_ADC_REG_RANK_1,
    .SamplingTime = LL_ADC_SAMPLINGTIME_COMMON_1
  }
};

static UTIL_TIMER_Object_t batt_period_timer;
static UTIL_TIMER_Object_t batt_settle_timer;
static uint32_t batt_current_period;

static void Batt_Read_Task(void);
static void Batt_Period_Cb(void *arg);
static void Batt_Settle_Cb(void *arg);
static uint8_t batt_voltage_to_percent(float v);
static uint32_t batt_get_period(uint8_t pct);

void APP_BATTERY_Init(void)
{
  ADCCTRL_Cmd_Status_t status;

  /* Register ADC handle with the controller */
  status = ADCCTRL_RegisterHandle(&BatteryRequest_Handle);
  if (status != ADCCTRL_OK)
  {
    LOG_INFO_APP(">> BATT: ADCCTRL register failed (%d)\n", status);
    return;
  }

  /* Configure PA6 as analog input */
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_6;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* Register sequencer task */
  UTIL_SEQ_RegTask(1U << CFG_TASK_BATT_READ, UTIL_SEQ_RFU, Batt_Read_Task);

  /* Create timers */
  UTIL_TIMER_Create(&batt_period_timer, 0, UTIL_TIMER_PERIODIC, Batt_Period_Cb, NULL);
  UTIL_TIMER_Create(&batt_settle_timer, 0, UTIL_TIMER_ONESHOT, Batt_Settle_Cb, NULL);

  /* Start periodic battery reading (normal interval until first read adjusts it) */
  batt_current_period = BATT_PERIOD_NORMAL_MS;
  UTIL_TIMER_StartWithPeriod(&batt_period_timer, batt_current_period);

  /* Trigger immediate first read to determine actual battery level */
  HAL_GPIO_WritePin(BAT_Read_GPIO_Port, BAT_Read_Pin, GPIO_PIN_SET);
  UTIL_TIMER_StartWithPeriod(&batt_settle_timer, 5);

  LOG_INFO_APP(">> BATT: Battery monitoring initialized\n");
}

static void Batt_Period_Cb(void *arg)
{
  (void)arg;
  /* Enable voltage divider circuit */
  HAL_GPIO_WritePin(BAT_Read_GPIO_Port, BAT_Read_Pin, GPIO_PIN_SET);
  /* Wait 5ms for C7 (100nF) to settle through divider */
  UTIL_TIMER_StartWithPeriod(&batt_settle_timer, 5);
}

static void Batt_Settle_Cb(void *arg)
{
  (void)arg;
  UTIL_SEQ_SetTask(1U << CFG_TASK_BATT_READ, CFG_SEQ_PRIO_1);
}

static void Batt_Read_Task(void)
{
  uint16_t raw = 0;
  ADCCTRL_Cmd_Status_t status;

  /* Request ADC ON */
  status = ADCCTRL_RequestIpState(&BatteryRequest_Handle, ADC_ON);
  if (status != ADCCTRL_OK)
  {
    LOG_INFO_APP(">> BATT: ADC ON failed (%d)\n", status);
    HAL_GPIO_WritePin(BAT_Read_GPIO_Port, BAT_Read_Pin, GPIO_PIN_RESET);
    return;
  }

  /* Read ADC raw value */
  status = ADCCTRL_RequestRawValue(&BatteryRequest_Handle, &raw);

  /* Release ADC */
  ADCCTRL_RequestIpState(&BatteryRequest_Handle, ADC_OFF);

  /* Disable voltage divider to save power */
  HAL_GPIO_WritePin(BAT_Read_GPIO_Port, BAT_Read_Pin, GPIO_PIN_RESET);

  if (status == ADCCTRL_OK)
  {
    /* Convert raw ADC to battery voltage:
     * Vref = 3.3V, 12-bit ADC (4095), voltage divider ratio = 0.5
     * Vbat = (raw / 4095) * 3.3 * 2.0 */
    float batt_voltage = ((float)raw / 4095.0f) * 3.3f * 2.0f;

    uint8_t batt_pct = batt_voltage_to_percent(batt_voltage);

    /* Update advertising data */
    APP_LEAK_Update_Battery(batt_pct);

    /* Adapt read interval based on battery level */
    uint32_t new_period = batt_get_period(batt_pct);
    if (new_period != batt_current_period)
    {
      batt_current_period = new_period;
      UTIL_TIMER_Stop(&batt_period_timer);
      UTIL_TIMER_StartWithPeriod(&batt_period_timer, batt_current_period);
      LOG_INFO_APP(">> BATT: Read interval changed to %lums\n", batt_current_period);
    }

    /* Low battery alert management */
    if (batt_pct < 10)
    {
      APP_ALERT_Start_LowBatt();
    }
    else
    {
      APP_ALERT_Stop_LowBatt();
    }

    /* Integer millivolt print — avoids needing -u _printf_float (saves ~8 KB) */
    uint32_t batt_mv = (uint32_t)(batt_voltage * 1000.0f + 0.5f);
    LOG_INFO_APP(">> BATT: %d%% (%lu.%02luV, raw=%u)\n",
                 batt_pct, batt_mv / 1000UL, (batt_mv % 1000UL) / 10UL, raw);
  }
  else
  {
    LOG_INFO_APP(">> BATT: ADC read failed (%d)\n", status);
  }
}

/**
 * @brief  Convert battery voltage to percentage using piecewise linear interpolation.
 * @note   CR2032 coin cell: 3.0V nominal, 2.0V cutoff.
 *         TPS63900 buck-boost operates down to 1.8V input.
 *         Discharge curve mapped to CR2032 profile at ~0.2mA:
 *           2.0V =   0%  (TPS63900 lower limit with margin)
 *           2.5V =   5%  (deep discharge — replace soon)
 *           2.7V =  20%  (discharge knee — low battery warning)
 *           2.8V =  40%  (transitioning out of flat region)
 *           2.9V =  75%  (still in flat discharge region)
 *           3.0V = 100%  (fresh cell)
 * @param  v  Battery voltage in volts
 * @retval Battery percentage (0-100)
 */
static uint8_t batt_voltage_to_percent(float v)
{
  const float voltage_points[]  = {2.0f, 2.5f, 2.7f, 2.8f, 2.9f, 3.0f};
  const uint8_t percent_points[] = {0,    5,    20,   40,   75,   100};
  const int num_points = 6;

  if (v <= voltage_points[0])
  {
    return percent_points[0];
  }
  if (v >= voltage_points[num_points - 1])
  {
    return percent_points[num_points - 1];
  }

  for (int i = 0; i < num_points - 1; i++)
  {
    if (v >= voltage_points[i] && v <= voltage_points[i + 1])
    {
      float slope = (float)(percent_points[i + 1] - percent_points[i]) /
                    (voltage_points[i + 1] - voltage_points[i]);
      return percent_points[i] + (uint8_t)(slope * (v - voltage_points[i]));
    }
  }

  return 0;
}

/**
 * @brief  Get battery read period based on current percentage.
 * @param  pct  Battery percentage (0-100)
 * @retval Period in milliseconds
 */
static uint32_t batt_get_period(uint8_t pct)
{
  if (pct < 10)
  {
    return BATT_PERIOD_CRITICAL_MS;
  }
  else if (pct <= 20)
  {
    return BATT_PERIOD_LOW_MS;
  }
  else
  {
    return BATT_PERIOD_NORMAL_MS;
  }
}
