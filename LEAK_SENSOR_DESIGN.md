# eLeak Sensor — Firmware Design Document

## Hardware Overview

- **MCU**: STM32WBA5MMGH6TR (B-WBA5M-WPAN module)
- **Sensor Pin**: PC13 (MSense) — backup-domain pin (TAMP1/WKUP4)
- **Pull-up**: External 390 KOhm resistor to VCC (GPIO PUPDR ignored on backup-domain pins)
- **LR/1M Switch**: PB7 (LR_BUT) — selects Long Range (Coded PHY) or 1M Legacy mode
- **Battery ADC**: PA6 (ADC4_IN3) via voltage divider, enabled by PB5 (BAT_Read)
- **Buzzer**: PB4 (Buzze_Pin) — GPIO push-pull, active HIGH
- **Alert LED**: PB3 (Alarm_Led_Pin) — GPIO push-pull, active HIGH

### Sensor Principle

Two PCB probes are connected between PC13 and GND. When dry, the external 390K
pull-up holds PC13 HIGH. When water bridges the probes, the resistance drops and
PC13 is pulled LOW.

## Detection Algorithm — EXTI-Based

### Flow

```
[CubeMX] PC13 = GPIO_MODE_IT_RISING_FALLING, priority 0
    |
[USER CODE MX_GPIO_Init_2] Lower EXTI13 priority to 6
    |
[APP_LEAK_Init]
    |-- Start power-up beep (500 ms)
    |-- Check initial pin state (leak at boot?)
    |-- Set init_done = 1 -> EXTI callbacks now process edges
    |
[EXTI13 fires on edge] (ISR)
    |-- if (!init_done) return  // pre-init guard
    |-- Disable EXTI13 NVIC (prevent storm)
    |-- Start 50 ms debounce timer
    |
[Timer fires after 50 ms] (timer ISR)
    |-- Schedule sequencer task
    |
[Leak_Process_Task] (main loop)
    |-- Read PC13 pin level
    |-- LOW = leak:  update adv data, start alert
    |-- HIGH = clear: update adv data, stop alert
    |-- Clear EXTI13 pending flags
    |-- Re-enable EXTI13 NVIC
```

### Why EXTI13 Is Disabled During Debounce

BLE TX at +10 dBm causes ~1.5 ms LOW glitches on PC13 through RF coupling
into the weak 390K pull-up. Without protection, each advertising event would
trigger an EXTI interrupt storm. By disabling EXTI13 NVIC immediately in the
ISR and only re-enabling it after the debounce task completes, we guarantee:

1. At most ONE interrupt per real edge event
2. No false triggers from RF coupling (50 ms >> 1.5 ms glitch)
3. The pin level is read in settled state after debounce

### EXTI13 Priority

CubeMX sets EXTI13 to priority 0 — same as `RADIO_INTR_PRIO_HIGH`. This
causes the EXTI ISR to preempt or compete with the BLE radio stack. The USER
CODE in `MX_GPIO_Init_2` lowers it to priority 6 (radio=0, RCC=1, BLE stack
priorities are 0-5 range).

### Standby / Stop1 Behavior

- **Stop1**: EXTI13 can wake the MCU from Stop1. After wakeup, the EXTI ISR
  fires, starts debounce, and the sequencer processes the event normally.
- **Standby Exit**: `MX_StandbyExit_PeripheralInit()` calls `MX_GPIO_Init()`
  which reconfigures PC13 as EXTI rising/falling. USER CODE lowers priority.

## Critical: BSP Button 2 on PC13

The B-WBA5M-WPAN BSP defines:

```c
#define B2_GPIO_PORT  GPIOC
#define B2_PIN        GPIO_PIN_13
#define B2_EXTI_LINE  EXTI_LINE_13
#define B2_EXTI_IRQn  EXTI13_IRQn
```

If `CFG_BUTTON_SUPPORTED = 1`, `APP_BSP_ButtonInit()` reconfigures PC13 as a
button and registers its own EXTI callback. The BSP callback calls
`APP_BSP_Button2Action()` which invokes `aci_gap_clear_security_db()`. This
completely hijacks the leak detection interrupt.

**FIX**: `CFG_BUTTON_SUPPORTED` is set to `0` in the USER CODE Defines block
of `app_conf.h`. This compiles out all BSP button initialization code.

## BLE Advertising

### PHY Selection (LR_BUT Switch)

| LR_BUT Pin | Mode | Advertising Type | PHY |
|------------|------|-----------------|-----|
| GND (LOW)  | Long Range | Extended, non-connectable, non-scannable | Coded S=8 |
| Float (HIGH) | Legacy | ADV_SCAN_IND, non-connectable, scannable | 1M |

Toggling the switch triggers EXTI7 -> 50 ms debounce -> `NVIC_SystemReset()`.
The new mode is read at boot from the pin level.

### Advertising Data (23 bytes)

| Offset | Length | Type | Content |
|--------|--------|------|---------|
| 0-2    | 3      | TX Power Level | +10 dBm (0x1F) |
| 3-9    | 7      | Complete Local Name | "eleak" |
| 10-13  | 4      | Appearance | Generic Sensor (0x0180) |
| 14-22  | 9      | Manufacturer Specific | See below |

**Manufacturer Specific Data (offset 14-22):**

| Byte | Content |
|------|---------|
| 14   | Length: 0x08 |
| 15   | Type: 0xFF (Mfg Specific) |
| 16-17 | Company ID: 0x0030 (ST Micro, little-endian) |
| 18   | **Leak status**: 0 = dry, 1 = leak |
| 19   | **Battery %**: 0-100 |
| 20   | FW Major version |
| 21   | FW Minor version |
| 22   | FW Patch version |

### Advertising Parameters

- Interval: 312.5 ms to 437.5 ms (500-700 in 0.625 ms units)
- Channels: 37, 38, 39 (all primary channels)
- TX Power: +10 dBm
- Duration: Continuous (no timeout)

## Alert System

### Power-Up Beep

A 500 ms buzzer + LED pulse at boot indicates the device has powered on.
Implemented with a one-shot timer in `APP_LEAK_Init()`. If a leak is already
present at boot, the leak alert takes over when the power-up beep ends.

### Leak Alert

- **Pattern**: 500 ms sustained beep every 5 seconds
- **Start**: `APP_ALERT_Start_Leak()` — fires immediate first beep, then 5s periodic
- **Stop**: `APP_ALERT_Stop()` — when leak clears
- **Outputs**: Buzzer (PB4) + LED (PB3) driven simultaneously

### Low Battery Alert

- **Pattern**: Double 100 ms beep (beep-pause-beep) every 10 seconds
- **Priority**: Suppressed while leak alert is active (leak takes precedence)
- **Trigger**: Battery monitor module calls `APP_ALERT_Start_LowBatt()` / `Stop_LowBatt()`

## Battery Monitor

Handled by `app_battery.c`. Reads ADC4_IN3 (PA6) via voltage divider, enabled
by BAT_Read (PB5). Updates advertising data byte 19. **Do not modify** — this
module works correctly.

## File Map

| File | Layer | Purpose |
|------|-------|---------|
| `STM32_WPAN/App/app_leak_detection.c` | App | EXTI detection, BLE advertising, init |
| `STM32_WPAN/App/app_leak_detection.h` | App | Public API |
| `STM32_WPAN/App/app_alert.c` | App | Buzzer/LED alert patterns |
| `STM32_WPAN/App/app_alert.h` | App | Alert API |
| `STM32_WPAN/App/app_battery.c` | App | Battery ADC + advertising update |
| `Core/Src/main.c` | Core | GPIO init (USER CODE: priority override) |
| `Core/Src/stm32wbaxx_it.c` | Core | ISR routing (USER CODE: EXTI callbacks) |
| `Core/Inc/app_conf.h` | Core | Config (USER CODE: CFG_BUTTON_SUPPORTED=0) |
| `System/Config/LowPower/peripheral_init.c` | System | Standby exit re-init |
| `Drivers/BSP/B-WBA5M-WPAN/b_wba5m_wpan.h` | BSP | B2_PIN=GPIO_PIN_13 (do not modify) |

## USER CODE Sections (CubeMX-Safe)

All custom code is within `USER CODE BEGIN/END` blocks:

1. **main.c** `USER CODE BEGIN SysInit`: clears stale PWR wake-up pin enables
2. **main.c** `MX_GPIO_Init_2`: EXTI13/EXTI7 priority override to 6, TAMP_IRQn mask
3. **stm32wbaxx_it.c** `Includes`: `#include "app_leak_detection.h"`
4. **stm32wbaxx_it.c** `1`: Rising/Falling EXTI callbacks
5. **app_conf.h** `Defines`: `CFG_BUTTON_SUPPORTED (0)` + `#ifdef DEBUG` LPM switch
6. **app_conf.h** `CFG_LPM_Id_t`: `CFG_LPM_BUZZER` enum entry
7. **app_conf.h** `CFG_Task_Id_t`: leak / alert / battery task IDs
8. **peripheral_init.c** `MX_STANDBY_EXIT_PERIPHERAL_INIT_2`: BSP button init removed

Standalone files (`app_leak_detection.c/h`, `app_alert.c/h`, `app_battery.c/h`)
are not touched by CubeMX code generation.

## Build Configurations (mirrors valve firmware pattern)

A single `#ifdef DEBUG` switch in `app_conf.h` USER CODE Defines drives the
build-time overrides; CubeMX regen preserves it.

| Config  | DEBUG defined | CFG_LPM_LEVEL | Logs | Debugger | Power | Use |
|---------|---------------|---------------|------|----------|-------|-----|
| Debug   | Yes           | 0 (Run/Sleep) | ON   | ON       | High (~250 µA idle) | Development, UART logs, SWD attach |
| Release | No            | 2 (Stop1)     | OFF  | OFF      | Low (target ~5 µA idle) | Power measurement, production flashing |

Workflow:
1. **STM32CubeIDE → Project → Build Configurations → Set Active**
2. Pick **Debug** for iteration with logs / debugger.
3. Pick **Release** before flashing for power measurement or shipping.
4. Logs only appear in Debug builds. SWD attach only works in Debug builds.

When `CFG_LPM_LEVEL` is set to 2 (Release), the auto-overrides further down in
`app_conf.h` (originally CubeMX-generated) zero out `CFG_LOG_SUPPORTED`,
`CFG_DEBUGGER_LEVEL`, and `CFG_LED_SUPPORTED` — that's what actually unlocks
Stop1 entry. Without disabling DBGMCU, the chip never reaches Stop1.

## LPM Buzzer Hold (CFG_LPM_BUZZER)

`Stop1` retains GPIO state in theory, but on this hardware the buzzer's
clock-domain transition during Stop1 entry / exit caused the audio output
to drop. Solution: a dedicated LPM client (`CFG_LPM_BUZZER`) blocks Stop1
while the buzzer is actively driven and releases it when the buzzer is off.

| Buzzer state | LPM hold |
|--------------|----------|
| Power-up beep ON (500 ms at boot) | `UTIL_LPM_SetMaxMode(CFG_LPM_BUZZER, UTIL_LPM_SLEEP_MODE)` |
| Power-up beep OFF (timer fires) | `UTIL_LPM_SetMaxMode(CFG_LPM_BUZZER, UTIL_LPM_MAX_MODE)` |
| Leak alert ON (every 5 s for 500 ms) | block (Alert_Tick_Task) |
| Leak alert OFF | release (Alert_Off_Task / APP_ALERT_Stop) |
| Low-batt double beep (start) | block (LowBatt_Beep_Task case 0) |
| Low-batt double beep (end) | release (LowBatt_Beep_Task case 3) |

Holding `UTIL_LPM_SLEEP_MODE` keeps the chip in Sleep mode (CPU off,
peripherals running) instead of Stop1 — clean buzzer drive at low cost
since the hold lasts only the few-hundred-ms beep window.

## Sequencer Tasks

| Task ID | Function | Trigger |
|---------|----------|---------|
| CFG_TASK_LEAK_PROCESS | `Leak_Process_Task` | EXTI debounce timer |
| CFG_TASK_LR_SWITCH | `LR_Switch_Task` | LR_BUT debounce timer |
| CFG_TASK_ALERT_TICK | `Alert_Tick_Task` | Alert period timer (5s) |
| CFG_TASK_ALERT_OFF | `Alert_Off_Task` | Alert on-time timer (500ms) |
| CFG_TASK_BATT_READ | Battery read task | Battery period timer |
| CFG_TASK_LOWBATT_BEEP | `LowBatt_Beep_Task` | Low-batt period timer (10s) |

## Known Issues & Historical Notes

### TAMP1 Approach (Abandoned)

The TAMP peripheral was tested as an alternative to EXTI for hardware-filtered
edge detection on PC13. Despite correct register configuration (CR1, CR2, FLTCR,
IER all verified), TAMP1 never triggered on the WBA5M module — not even with a
HIGHLEVEL diagnostic trigger when the pin was demonstrably HIGH. Root cause
unknown. The EXTI approach was adopted instead.

### Previous EXTI Failures (4 iterations)

Four EXTI-based implementations were attempted in earlier sessions, all failing.
The root cause was discovered later: `B2_PIN = GPIO_PIN_13` in the BSP header.
With `CFG_BUTTON_SUPPORTED=1`, `APP_BSP_ButtonInit()` silently re-enabled EXTI13
as Button 2, intercepting all leak sensor events as button presses. Setting
`CFG_BUTTON_SUPPORTED=0` eliminates this interference.

Additional contributing factors in the earlier failures:
- EXTI13 priority 0 (same as radio) — now lowered to 6
- No EXTI disable during debounce — caused interrupt storms from RF coupling
- RF coupling from BLE TX (+10 dBm) on weak 390K pull-up — handled by debounce
