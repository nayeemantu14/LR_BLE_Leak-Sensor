#include "app_alert.h"
#include "main.h"
#include "app_common.h"
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "stm32_lpm.h"
#include "stm32_lpm_if.h"
#include "app_conf.h"
#include "log_module.h"

/* Block Stop1 while a buzzer tone is being driven.  Stop1 gates the
 * high-frequency clocks; on this hardware the magnetic indicator's audio
 * output suffers across Stop1 entry/exit transients.  Holding
 * UTIL_LPM_SLEEP_MODE keeps the chip in Sleep (CPU off, peripherals
 * running) for the few-tens-of-ms tone window.  Released during
 * inter-pulse and inter-cycle gaps so Stop1 remains reachable. */
#define BUZZER_LPM_BLOCK()    UTIL_LPM_SetMaxMode(1U << CFG_LPM_BUZZER, UTIL_LPM_SLEEP_MODE)
#define BUZZER_LPM_RELEASE()  UTIL_LPM_SetMaxMode(1U << CFG_LPM_BUZZER, UTIL_LPM_MAX_MODE)

void APP_ALERT_BlockLPM(void)   { BUZZER_LPM_BLOCK(); }
void APP_ALERT_ReleaseLPM(void) { BUZZER_LPM_RELEASE(); }

/* ========== Declarative beep patterns ==========
 *
 * Buzzer is a CMI-9605IC-0380T: internally-driven magnetic indicator,
 * 2.7 kHz internal oscillator, 30 mA at 3 V.  Drive method is DC on/off;
 * hardware PWM at 2.7 kHz starved the internal driver (silent).  This
 * "soft-PWM" approach instead chops the DC supply at a much slower
 * envelope (50-250 Hz) — each ON segment is long enough for the internal
 * 2.7 kHz oscillator to start ringing, the OFF segment cuts current draw
 * in half.  Audible result: 2.7 kHz tone with envelope buzz on top.
 *
 * Tuning knobs (all three options give same burst duration + same total
 * ON time; only the envelope frequency changes — try all three):
 *
 *   A) Slow chop:   on=10, off=10, repeat=10   → 50 Hz envelope
 *   B) Medium chop: on=5,  off=5,  repeat=20   → 100 Hz envelope
 *   C) Fast chop:   on=2,  off=2,  repeat=50   → 250 Hz envelope
 *
 * Pick whichever sounds best (subjectively or via mic) and stick with it.
 */
typedef struct {
  uint16_t on_ms;       /* tone duration */
  uint16_t off_ms;      /* silence inside a burst */
  uint8_t  repeat;      /* number of pulses per burst */
  uint16_t cycle_ms;    /* time between burst starts; 0 = single-shot */
} beep_pattern_t;

/* Soft-PWM leak alert.  20% duty / 100 Hz envelope — keeps each ON pulse
 * short enough (2 ms × 30 mA = 60 µC) that bulk cap absorbs most of it,
 * with an 8 ms recovery gap for the boost to refill C6/C9.  Audible: each
 * 2 ms pulse rings ~5 cycles of the internal 2.7 kHz oscillator. */
/* All three patterns use the same 2 ms / 8 ms soft-PWM chop (20% duty,
 * 100 Hz envelope).  Audibility distinction comes from total burst length:
 * leak = 200 ms, low-batt = 100 ms, power-up = 80 ms.
 *
 * NOTE: on_ms must stay >= 2.  At on_ms=1 the UTIL_TIMER alarm hits its
 * 3-tick RTC-subsecond floor and the sequencer rapid-fires the pattern
 * tick task hard enough to starve other tasks (logs stop arriving in
 * Debug builds — confirmed empirically). */
static const beep_pattern_t pattern_leak    = { .on_ms = 2, .off_ms = 8, .repeat = 20, .cycle_ms = 8000  };
static const beep_pattern_t pattern_lowbatt = { .on_ms = 2, .off_ms = 8, .repeat = 10, .cycle_ms = 30000 };
static const beep_pattern_t pattern_powerup = { .on_ms = 2, .off_ms = 8, .repeat = 8,  .cycle_ms = 0     };

/* ========== Engine state ========== */

typedef enum {
  PH_IDLE,         /* nothing playing; no LPM hold */
  PH_TONE_ON,      /* tone driving; timer running for on_ms */
  PH_TONE_OFF,     /* gap inside a burst; timer running for off_ms */
  PH_CYCLE_WAIT,   /* between bursts; LPM released; timer running for the gap */
} phase_t;

static const beep_pattern_t *cur_pattern;
static phase_t  cur_phase;
static uint8_t  cur_pulses_done;

static UTIL_TIMER_Object_t pattern_timer;

/* Source-of-truth alert flags.  Engine plays the highest-priority active
 * pattern when (re)starting a cycle.  Priority: Leak > LowBatt.
 * Power-up is a one-shot that runs only when the engine is IDLE. */
static uint8_t leak_active;
static uint8_t lowbatt_active;

static void Pattern_Tick_Task(void);
static void Pattern_Timer_Cb(void *arg);

static void engine_drive(uint8_t on);
static const beep_pattern_t *engine_select_pattern(void);
static void engine_start_burst(const beep_pattern_t *p);

/* ========== Initialization ========== */

void APP_ALERT_Init(void)
{
  UTIL_SEQ_RegTask(1U << CFG_TASK_PATTERN_TICK, UTIL_SEQ_RFU, Pattern_Tick_Task);
  UTIL_TIMER_Create(&pattern_timer, 0, UTIL_TIMER_ONESHOT, Pattern_Timer_Cb, NULL);

  cur_pattern = NULL;
  cur_phase = PH_IDLE;
  cur_pulses_done = 0;
  leak_active = 0;
  lowbatt_active = 0;
}

/* ========== Public API ========== */

void APP_ALERT_Start_Leak(void)
{
  if (leak_active)
  {
    return;
  }
  leak_active = 1;
  LOG_INFO_APP(">> ALERT: Leak alert started\n");

  /* Leak preempts anything currently playing (power-up beep or low-batt). */
  UTIL_TIMER_Stop(&pattern_timer);
  engine_start_burst(&pattern_leak);
}

void APP_ALERT_Stop(void)
{
  if (!leak_active)
  {
    return;
  }
  leak_active = 0;
  LOG_INFO_APP(">> ALERT: Leak alert stopped\n");

  /* If we're currently playing the leak pattern, stop it and let
   * low-batt resume if it was suppressed.  If we were between leak
   * cycles, the wait timer is still running — kill it and reselect. */
  if (cur_pattern == &pattern_leak)
  {
    UTIL_TIMER_Stop(&pattern_timer);
    engine_drive(0);
    BUZZER_LPM_RELEASE();
    cur_pattern = NULL;
    cur_phase = PH_IDLE;

    const beep_pattern_t *next = engine_select_pattern();
    if (next != NULL)
    {
      engine_start_burst(next);
    }
  }
}

void APP_ALERT_Start_LowBatt(void)
{
  if (lowbatt_active)
  {
    return;
  }
  lowbatt_active = 1;
  LOG_INFO_APP(">> ALERT: Low battery alert started\n");

  /* Only start playing if no higher-priority pattern is on the bus. */
  if (!leak_active && cur_phase == PH_IDLE)
  {
    engine_start_burst(&pattern_lowbatt);
  }
}

void APP_ALERT_Stop_LowBatt(void)
{
  if (!lowbatt_active)
  {
    return;
  }
  lowbatt_active = 0;
  LOG_INFO_APP(">> ALERT: Low battery alert stopped\n");

  if (cur_pattern == &pattern_lowbatt)
  {
    UTIL_TIMER_Stop(&pattern_timer);
    engine_drive(0);
    BUZZER_LPM_RELEASE();
    cur_pattern = NULL;
    cur_phase = PH_IDLE;
  }
}

void APP_ALERT_PowerUpBeep(void)
{
  /* One-shot indication beep.  Skipped if anything is already playing.
   * If a leak is detected immediately after this call, APP_ALERT_Start_Leak
   * will preempt the burst — that's expected and handled cleanly. */
  if (cur_phase != PH_IDLE)
  {
    return;
  }
  engine_start_burst(&pattern_powerup);
}

/* ========== Engine internals ========== */

static void engine_drive(uint8_t on)
{
  GPIO_PinState s = on ? GPIO_PIN_SET : GPIO_PIN_RESET;
  HAL_GPIO_WritePin(Buzze_GPIO_Port, Buzze_Pin, s);
  HAL_GPIO_WritePin(Alarm_Led_GPIO_Port, Alarm_Led_Pin, s);
}

static const beep_pattern_t *engine_select_pattern(void)
{
  /* Priority order. Power-up is one-shot and not part of resume. */
  if (leak_active)    return &pattern_leak;
  if (lowbatt_active) return &pattern_lowbatt;
  return NULL;
}

static void engine_start_burst(const beep_pattern_t *p)
{
  cur_pattern = p;
  cur_pulses_done = 0;
  cur_phase = PH_TONE_ON;
  BUZZER_LPM_BLOCK();
  engine_drive(1);
  UTIL_TIMER_StartWithPeriod(&pattern_timer, p->on_ms);
}

static void Pattern_Timer_Cb(void *arg)
{
  (void)arg;
  UTIL_SEQ_SetTask(1U << CFG_TASK_PATTERN_TICK, CFG_SEQ_PRIO_1);
}

static void Pattern_Tick_Task(void)
{
  if (cur_pattern == NULL)
  {
    return;
  }
  const beep_pattern_t *p = cur_pattern;

  switch (cur_phase)
  {
    case PH_TONE_ON:
      /* Tone window just ended. */
      engine_drive(0);
      cur_pulses_done++;

      if (cur_pulses_done >= p->repeat)
      {
        /* Burst complete.  Release LPM so we can sleep through the gap. */
        BUZZER_LPM_RELEASE();

        if (p->cycle_ms == 0)
        {
          /* Single-shot done.  Hand back to any pending alert. */
          cur_pattern = NULL;
          cur_phase = PH_IDLE;

          const beep_pattern_t *next = engine_select_pattern();
          if (next != NULL)
          {
            engine_start_burst(next);
          }
        }
        else
        {
          /* Periodic — wait the inter-cycle gap. */
          uint32_t burst_dur = (uint32_t)p->repeat * ((uint32_t)p->on_ms + (uint32_t)p->off_ms);
          uint32_t wait = (p->cycle_ms > burst_dur) ? (p->cycle_ms - burst_dur) : 1U;
          cur_phase = PH_CYCLE_WAIT;
          UTIL_TIMER_StartWithPeriod(&pattern_timer, wait);
        }
      }
      else
      {
        /* More pulses in this burst — short gap. */
        cur_phase = PH_TONE_OFF;
        UTIL_TIMER_StartWithPeriod(&pattern_timer, p->off_ms);
      }
      break;

    case PH_TONE_OFF:
      /* Intra-burst gap done — start the next pulse. */
      cur_phase = PH_TONE_ON;
      engine_drive(1);
      UTIL_TIMER_StartWithPeriod(&pattern_timer, p->on_ms);
      break;

    case PH_CYCLE_WAIT:
    {
      /* Inter-cycle wait done.  Priority may have changed while sleeping
       * (e.g., leak ended, low-batt should resume).  Reselect. */
      const beep_pattern_t *next = engine_select_pattern();
      if (next == NULL)
      {
        cur_pattern = NULL;
        cur_phase = PH_IDLE;
      }
      else
      {
        engine_start_burst(next);
      }
      break;
    }

    default:
      break;
  }
}
