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
#define ELEAK_FW_MINOR  1
#define ELEAK_FW_PATCH  0

/* Debounce period in ms.  Must exceed the longest RF coupling glitch
 * (~1.5 ms from BLE TX at +10 dBm on the weak 390 KOhm pull-up). */
#define LEAK_DEBOUNCE_MS  50

/* ---------------------------------------------------------------------------
 * Power management (Gate 4 redesign) — Phase 2: decoupled leak-edge burst
 *
 * On a leak-state change we fire a bounded advertising "burst" on handle 0 in
 * addition to refreshing the (still continuous) advertising payload.  The
 * burst is the primitive the event-driven model will rely on once continuous
 * advertising is removed (Phase 3b); in Phase 2 continuous advertising is
 * still running, so the burst is deliberately redundant.  Its purpose here is
 * to prove the Duration-bounded enable + HCI_LE_ADVERTISING_SET_TERMINATED
 * path on the Basic-Plus stack and to measure the single-advert miss
 * probability (q) — before the continuous safety net is removed.
 *
 * ELEAK_ADV_CONTINUOUS_BASELINE selects the resting state the terminated
 * handler returns to after a burst:
 *   1  (Phase 2 / 3a) -> re-arm continuous advertising (Duration = 0).
 *   0  (Phase 3b)     -> rest in Stop1 idle (no advertising).
 * This is the single switch that performs the Phase 3b cutover.
 *
 * Phase 3b (= 0, ACTIVE): continuous advertising is removed.  Advertising
 * happens ONLY in bounded bursts — the periodic heartbeat (~100 s), the leak
 * edge, and an immediate boot burst — and the MCU idles in Stop1 between them.
 * Battery-% updates ride the next heartbeat (<=100 s latency, non-critical) so
 * no separate battery burst is needed; the async boot battery read lands
 * in-place during the boot burst.  Rollback = set this back to 1 (continuous
 * restored exactly; heartbeat timer becomes a harmless redundant re-arm). */
#define ELEAK_ADV_CONTINUOUS_BASELINE  0

/* Leak-edge burst length in 10 ms units (Adv_Set_t.Duration).
 * ~4 s at the 312.5–437.5 ms interval ≈ 9–12 advertising events. */
#define ELEAK_LEAK_BURST_DURATION_X10MS  400U

/* ---------------------------------------------------------------------------
 * Power management (Gate 4 redesign) — Phase 3a: periodic heartbeat burst
 *
 * A periodic LSE-backed timer emits a short bounded advertising burst so the
 * hub keeps the sensor "online" even when nothing changes.  The hub re-stamps
 * its health last_seen only when it receives an advert inside its 5-min
 * BLE_LEAK_HEARTBEAT window, so the sensor must land a *received* advert well
 * inside every 5 min.  Period 100 s (3 transmit windows per 5-min hub window)
 * with a burst long enough for N>=3 advertising events gives strong reception
 * redundancy against the hub's ~50%-duty passive scan.  Tuned in Phase 4 from
 * the measured single-advert miss probability q.
 *
 * The heartbeat ALSO re-reads the MSense pin each tick (eleak_reconcile_leak_
 * state) so a single lost dry->wet edge self-heals — it is the periodic
 * ground-truth re-sample, not just a cached re-broadcast. */
#define ELEAK_HEARTBEAT_PERIOD_MS  100000U   /* 100 s — quiescent (dry) cadence */

/* Faster heartbeat while a leak is asserted.  During a leak the byte is on the
 * air only at the onset burst + each heartbeat; a slow cadence would leave a
 * up-to-period window where a freshly-rebooted hub cannot re-learn leak=1.
 * 15 s bounds that re-learn gap; a leak is rare/bounded so the energy cost on
 * the 2-yr budget is negligible (and the buzzer dominates during a leak). */
#define ELEAK_HEARTBEAT_LEAK_PERIOD_MS  15000U   /* 15 s — leak-asserted cadence */

/* Heartbeat burst length in 10 ms units.  ~2.5 s at the 312.5–437.5 ms
 * interval yields >=6 advertising events even at the interval ceiling (each
 * event = 3 channels).  Sized so that one — and even two consecutive — missed
 * bursts cannot push the hub past its 10-min offline timeout (the false-offline
 * margin; see PHASE3B_NOTES "no-false-offline" math).  Finalised in Phase 4
 * from the measured single-advert miss probability q. */
#define ELEAK_HEARTBEAT_BURST_DURATION_X10MS  250U

/* Burst enable-failure retry: Phase 3b removed the continuous carrier, so a
 * bounded enable is the ONLY thing that puts the (possibly leak=1) payload on
 * the air.  If aci_gap_adv_set_enable returns non-success we must not silently
 * drop the report — retry a few times on a short one-shot timer.  The heartbeat
 * re-sample is the slower backstop if retries are exhausted. */
#define ELEAK_BURST_RETRY_MS      100U
#define ELEAK_BURST_MAX_RETRIES   3U

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
  ELEAK_FW_MAJOR, ELEAK_FW_MINOR, ELEAK_FW_PATCH /* FW version 1.1.0 */
};

static Adv_Set_t eleak_adv_set;
static UTIL_TIMER_Object_t leak_debounce_timer;
static UTIL_TIMER_Object_t lr_debounce_timer;
static UTIL_TIMER_Object_t leak_heartbeat_timer;   /* Phase 3a periodic heartbeat */
static UTIL_TIMER_Object_t burst_retry_timer;      /* one-shot burst enable retry */
static uint8_t leak_state;        /* 0 = no leak, 1 = leak */
static uint8_t a_peeraddr[8];
static volatile uint8_t init_done; /* gates EXTI callbacks until APP_LEAK_Init completes */
static uint16_t s_pending_burst_dur;   /* duration (x10ms) of the in-flight burst, for retry */
static uint8_t  s_burst_retry_count;   /* consecutive enable-failure retries */
static uint32_t s_heartbeat_period_ms; /* current armed heartbeat period (dry vs leak) */

static void Leak_Process_Task(void);
static void LR_Switch_Task(void);
static void Leak_Debounce_Cb(void *arg);
static void LR_Debounce_Cb(void *arg);
static void Leak_Heartbeat_Task(void);
static void Leak_Heartbeat_Cb(void *arg);
static void Burst_Retry_Task(void);
static void Burst_Retry_Cb(void *arg);
static uint8_t eleak_reconcile_leak_state(void);
static void eleak_set_heartbeat_period(void);
#if ELEAK_ADV_CONTINUOUS_BASELINE
static tBleStatus eleak_enable_continuous(void);
#endif
static void eleak_start_burst(uint16_t duration_x10ms);
static void eleak_attempt_burst(void);

void APP_LEAK_Init(void)
{
  tBleStatus status;

  /* Register sequencer tasks */
  UTIL_SEQ_RegTask(1U << CFG_TASK_LEAK_PROCESS, UTIL_SEQ_RFU, Leak_Process_Task);
  UTIL_SEQ_RegTask(1U << CFG_TASK_LR_SWITCH, UTIL_SEQ_RFU, LR_Switch_Task);
  UTIL_SEQ_RegTask(1U << CFG_TASK_LEAK_HEARTBEAT, UTIL_SEQ_RFU, Leak_Heartbeat_Task);
  UTIL_SEQ_RegTask(1U << CFG_TASK_BURST_RETRY, UTIL_SEQ_RFU, Burst_Retry_Task);

  /* Create debounce timers */
  UTIL_TIMER_Create(&leak_debounce_timer, 0, UTIL_TIMER_ONESHOT, Leak_Debounce_Cb, NULL);
  UTIL_TIMER_Create(&lr_debounce_timer, 0, UTIL_TIMER_ONESHOT, LR_Debounce_Cb, NULL);

  /* Phase 3a: periodic heartbeat burst timer (started after advertising is up). */
  UTIL_TIMER_Create(&leak_heartbeat_timer, 0, UTIL_TIMER_PERIODIC, Leak_Heartbeat_Cb, NULL);

  /* One-shot retry timer for a failed burst enable (Phase 3b: no continuous fallback). */
  UTIL_TIMER_Create(&burst_retry_timer, 0, UTIL_TIMER_ONESHOT, Burst_Retry_Cb, NULL);

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

#if ELEAK_ADV_CONTINUOUS_BASELINE
  /* Phase 2/3a: continuous advertising is the resting state (Duration = 0).
   * Leak/heartbeat bursts temporarily re-bound this set, and the terminated
   * handler re-arms continuous afterwards. */
  status = eleak_enable_continuous();

  if (status == BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP(">> LEAK: Advertising started (continuous)\n");
  }
  else
  {
    LOG_INFO_APP(">> LEAK: aci_gap_adv_set_enable FAILED 0x%02X\n", status);
  }
#else
  /* Phase 3b: NO continuous advertising.  The adv set stays configured but
   * disabled; advertising happens only in bounded bursts (boot burst below,
   * heartbeat, leak edge), idling in Stop1 between.  status is unused here but
   * was already consumed by the set-config / set-adv-data calls above. */
  (void)status;
  LOG_INFO_APP(">> LEAK: Continuous adv disabled — burst-only / Stop1 idle\n");
#endif

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

  /* Re-sample the pin now that callbacks are unblocked: an edge that arrived
   * between the boot read above and init_done was dropped (HAL/NVIC pending was
   * just cleared).  Reconcile catches a sustained level change so the boot
   * burst below reflects ground truth (closes the boot-window lost-edge race). */
  (void)eleak_reconcile_leak_state();

  /* Phase 3a: arm the periodic heartbeat at the cadence matching leak state
   * (faster while a leak is asserted). */
  eleak_set_heartbeat_period();

#if !ELEAK_ADV_CONTINUOUS_BASELINE
  /* Phase 3b: no continuous carrier — advertise the boot state immediately
   * instead of waiting a full heartbeat period.  Carries the initial leak byte
   * (incl. wet-at-boot, where no EXTI edge will fire) and the default battery;
   * the async first battery read lands in-place during this burst.  A
   * wet-at-boot state uses the longer leak-burst duration for max reception
   * redundancy on the safety-critical case. */
  eleak_start_burst(leak_state ? ELEAK_LEAK_BURST_DURATION_X10MS
                               : ELEAK_HEARTBEAT_BURST_DURATION_X10MS);
#endif

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
 * @brief  Re-read MSense and reconcile leak_state / payload / alert / cadence.
 * @retval 1 if the leak state changed, 0 otherwise.
 *
 * Shared ground-truth evaluation used by both the EXTI/debounce path
 * (Leak_Process_Task) and the periodic heartbeat re-sample (Leak_Heartbeat_Task).
 *   LOW  = water bridging probes = leak detected
 *   HIGH = external pull-up = dry = no leak
 * Does NOT itself advertise — the caller decides whether/how to burst — but it
 * does update the adv payload, the alert, and the heartbeat cadence so a single
 * lost dry->wet edge self-heals at the next heartbeat tick.
 */
static uint8_t eleak_reconcile_leak_state(void)
{
  GPIO_PinState pin = HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin);
  uint8_t new_state = (pin == GPIO_PIN_RESET) ? 1 : 0;

  if (new_state == leak_state)
  {
    return 0;
  }

  leak_state = new_state;
  a_EleakAdvData[18] = leak_state;

  aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                           sizeof(a_EleakAdvData), a_EleakAdvData);

  if (leak_state)
  {
    APP_ALERT_Start_Leak();
    LOG_INFO_APP(">> LEAK: LEAK DETECTED (MSense LOW)\n");
  }
  else
  {
    APP_ALERT_Stop();
    LOG_INFO_APP(">> LEAK: Leak cleared (MSense HIGH)\n");
  }

  /* Adapt heartbeat cadence to the new state (faster while leaking). */
  eleak_set_heartbeat_period();
  return 1;
}

/**
 * @brief  Leak process task — runs in main loop after the 50 ms debounce.
 *
 * Reconciles against the settled pin; on a state change fires the decoupled
 * bounded leak burst (the safety-critical onset/clear edge).  Then clears
 * EXTI13 pending flags and re-enables the NVIC so the next real edge is caught.
 */
static void Leak_Process_Task(void)
{
  if (eleak_reconcile_leak_state())
  {
    /* Onset/clear edge — fire the longer leak burst for max reception
     * redundancy.  Post-cutover (Phase 3b) this is the sole carrier of the
     * edge; the heartbeat re-sample + burst-enable retry are the backstops. */
    eleak_start_burst(ELEAK_LEAK_BURST_DURATION_X10MS);
  }

  /* Re-enable EXTI13: clear pending flags first to avoid immediate re-trigger
   * from edges that occurred during the debounce window. */
  __HAL_GPIO_EXTI_CLEAR_IT(MSense_Pin);
  NVIC_ClearPendingIRQ(EXTI13_IRQn);
  HAL_NVIC_EnableIRQ(EXTI13_IRQn);

  /* An edge that landed between the reconcile pin-read and the flag-clear above
   * had its pending bit wiped with no callback.  If the settled level now
   * disagrees with leak_state, re-arm the debounce so the missed onset/clear is
   * caught in ~50 ms instead of only at the next heartbeat re-sample.  Re-running
   * through the debounce path (not an immediate re-schedule) bounds this against
   * a bouncing pin. */
  if ((uint8_t)(HAL_GPIO_ReadPin(MSense_GPIO_Port, MSense_Pin) == GPIO_PIN_RESET) != leak_state)
  {
    UTIL_TIMER_StartWithPeriod(&leak_debounce_timer, LEAK_DEBOUNCE_MS);
  }
}

static void LR_Switch_Task(void)
{
  LOG_INFO_APP(">> LEAK: LR switch toggled — resetting MCU\n");
  NVIC_SystemReset();
}

/**
 * @brief  (Re)arm the periodic heartbeat timer at the cadence for the current
 *         leak state — fast while leaking, slow when dry.
 * @note   Safe to call from a sequencer task (mirrors app_battery.c's adaptive
 *         period restart).  Only restarts when the period actually changes, so
 *         calling it every reconcile is cheap.
 */
static void eleak_set_heartbeat_period(void)
{
  uint32_t period = leak_state ? ELEAK_HEARTBEAT_LEAK_PERIOD_MS
                               : ELEAK_HEARTBEAT_PERIOD_MS;

  if (period == s_heartbeat_period_ms)
  {
    return;
  }

  s_heartbeat_period_ms = period;
  UTIL_TIMER_Stop(&leak_heartbeat_timer);
  UTIL_TIMER_StartWithPeriod(&leak_heartbeat_timer, period);
}

/**
 * @brief  Heartbeat period timer callback — fires every s_heartbeat_period_ms.
 * @note   Timer/ISR context — schedule the heartbeat sequencer task.
 */
static void Leak_Heartbeat_Cb(void *arg)
{
  (void)arg;
  UTIL_SEQ_SetTask(1U << CFG_TASK_LEAK_HEARTBEAT, CFG_SEQ_PRIO_1);
}

/**
 * @brief  Heartbeat task — periodic ground-truth re-sample + bounded burst.
 *
 * Re-reads MSense first (eleak_reconcile_leak_state) so a single lost dry->wet
 * edge self-heals here instead of advertising leak=0 forever; then emits a
 * bounded burst carrying the (reconciled) current payload so the hub keeps the
 * sensor online.  A reconcile that flips to wet uses the longer leak burst.
 */
static void Leak_Heartbeat_Task(void)
{
  if (eleak_reconcile_leak_state() && leak_state)
  {
    /* Heartbeat caught a missed dry->wet edge — treat it as an onset. */
    eleak_start_burst(ELEAK_LEAK_BURST_DURATION_X10MS);
  }
  else
  {
    eleak_start_burst(ELEAK_HEARTBEAT_BURST_DURATION_X10MS);
  }
}

void APP_LEAK_Update_Battery(uint8_t batt_pct)
{
  a_EleakAdvData[19] = batt_pct;
  aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                           sizeof(a_EleakAdvData), a_EleakAdvData);
}

/* ---------------------------------------------------------------------------
 * Advertising burst primitives (Gate 4 power redesign — Phase 2)
 * --------------------------------------------------------------------------- */

#if ELEAK_ADV_CONTINUOUS_BASELINE
/**
 * @brief  Re-arm the continuous (resting) advertising state on handle 0.
 * @note   Duration = 0 => advertise until explicitly disabled.
 *         Only used in the Phase 2/3a baseline (continuous); compiled out in
 *         the Phase 3b cutover where the resting state is Stop1 idle.
 */
static tBleStatus eleak_enable_continuous(void)
{
  eleak_adv_set.Advertising_Handle = 0;
  eleak_adv_set.Duration = 0;
  eleak_adv_set.Max_Extended_Advertising_Events = 0;
  return aci_gap_adv_set_enable(1, 1, &eleak_adv_set);
}
#endif

/**
 * @brief  Attempt the in-flight bounded burst (s_pending_burst_dur) on handle 0.
 *
 * The legacy 1M path bounds the burst with Duration (Max_Extended_Advertising_
 * Events is extended-adv-only and stays 0).  Expiry raises
 * HCI_LE_ADVERTISING_SET_TERMINATED -> APP_LEAK_OnAdvTerminated().  We disable
 * first so (Enable=1 + new Duration) restarts the bound unambiguously whether a
 * previous burst (or, pre-cutover, continuous adv) is currently active; a
 * host-commanded disable does NOT raise the terminated event, so exactly one
 * terminated event fires per burst (on Duration expiry).
 *
 * MUST-FIX (Phase 3b): the enable return code is checked.  Post-cutover this
 * enable is the ONLY thing that puts the (possibly leak=1) payload on the air,
 * so on failure we retry on a short one-shot timer up to ELEAK_BURST_MAX_RETRIES
 * rather than silently dropping the report (the next heartbeat re-sample is the
 * slower backstop).  Called both for a fresh burst (via eleak_start_burst) and
 * from the retry task (Burst_Retry_Task), which does not reset the counter.
 */
static void eleak_attempt_burst(void)
{
  tBleStatus st;

  /* Load the latest payload (leak byte / battery) before the burst. */
  aci_gap_adv_set_adv_data(0, HCI_SET_ADV_DATA_OPERATION_COMPLETE, 0,
                           sizeof(a_EleakAdvData), a_EleakAdvData);

  eleak_adv_set.Advertising_Handle = 0;
  eleak_adv_set.Duration = s_pending_burst_dur;
  eleak_adv_set.Max_Extended_Advertising_Events = 0;

  aci_gap_adv_set_enable(0, 1, &eleak_adv_set);          /* silent disable (rc ignored) */
  st = aci_gap_adv_set_enable(1, 1, &eleak_adv_set);     /* bounded (re)enable */

  if (st == BLE_STATUS_SUCCESS)
  {
    UTIL_TIMER_Stop(&burst_retry_timer);
    s_burst_retry_count = 0;
    return;
  }

  LOG_INFO_APP(">> LEAK: burst enable FAILED 0x%02X (attempt %u)\n", st, s_burst_retry_count);

  if (s_burst_retry_count < ELEAK_BURST_MAX_RETRIES)
  {
    s_burst_retry_count++;
    UTIL_TIMER_Stop(&burst_retry_timer);
    UTIL_TIMER_StartWithPeriod(&burst_retry_timer, ELEAK_BURST_RETRY_MS);
  }
  else
  {
    LOG_INFO_APP(">> LEAK: burst enable gave up after %u retries (heartbeat re-sample will retry)\n",
                 (unsigned)ELEAK_BURST_MAX_RETRIES);
    s_burst_retry_count = 0;
  }
}

/**
 * @brief  Fire a fresh bounded advertising burst on handle 0.
 * @param  duration_x10ms  Burst length in 10 ms units (Adv_Set_t.Duration).
 *
 * Resets the retry counter (this is a new logical burst) and attempts it;
 * eleak_attempt_burst handles the failure/retry path.
 */
static void eleak_start_burst(uint16_t duration_x10ms)
{
  s_pending_burst_dur = duration_x10ms;
  s_burst_retry_count = 0;
  eleak_attempt_burst();
}

/**
 * @brief  Burst-retry timer callback — schedules a re-attempt of the last burst.
 * @note   Timer/ISR context.  PRIO_0 so a failed safety-critical burst re-fires
 *         promptly.
 */
static void Burst_Retry_Cb(void *arg)
{
  (void)arg;
  UTIL_SEQ_SetTask(1U << CFG_TASK_BURST_RETRY, CFG_SEQ_PRIO_0);
}

/**
 * @brief  Burst-retry task — re-attempts the pending burst without resetting
 *         the retry counter (so the MAX_RETRIES cap is honoured).
 * @note   A successful interleaving burst may have stopped burst_retry_timer
 *         after Burst_Retry_Cb already queued this task; in that benign window
 *         we simply re-emit the current (identical) payload once.  The retry
 *         counter is 0 on that path, so this cannot extend the retry budget.
 */
static void Burst_Retry_Task(void)
{
  eleak_attempt_burst();
}

/**
 * @brief  HCI_LE_ADVERTISING_SET_TERMINATED handler for the leak adv set.
 * @param  adv_handle  Advertising set that terminated.
 * @param  status      Termination status from the controller.
 *
 * Phase 2 / 3a (ELEAK_ADV_CONTINUOUS_BASELINE = 1): re-arm continuous
 * advertising so the leak byte keeps broadcasting after the burst.
 * Phase 3b (= 0): rest in Stop1 idle — nothing to re-arm here.
 */
void APP_LEAK_OnAdvTerminated(uint8_t adv_handle, uint8_t status)
{
  if (adv_handle != 0)
  {
    return;
  }

  LOG_INFO_APP(">> LEAK: ADV burst terminated (status 0x%02X)\n", status);
  UNUSED(status);   /* LOG_INFO_APP compiles out in Release -> avoid -Wunused */

#if ELEAK_ADV_CONTINUOUS_BASELINE
  eleak_enable_continuous();
#endif
}
