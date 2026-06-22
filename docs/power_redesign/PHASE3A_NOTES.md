# Phase 3a — Periodic heartbeat burst timer (IMPLEMENTED)

> Scope per [GATE4_PLAN.md](GATE4_PLAN.md) Phase 3a. **Continuous advertising still runs** — the heartbeat
> is additive, isolating the heartbeat mechanism from the Phase 3b Stop1 cutover. Builds directly on the
> Phase 2 burst primitive (validated on hardware: terminated event fires with status `0x3C` = Advertising
> Timeout). No hub change. Payload byte-identical. TX power unchanged.

## What changed (sensor firmware only)

| File | Change |
|---|---|
| `Core/Inc/app_conf.h` | Appended `CFG_TASK_LEAK_HEARTBEAT` to `CFG_Task_Id_t` (USER CODE block). **Appended, not inserted** — the sequencer callback array is sized by `UTIL_SEQ_CONF_TASK_NBR` (fixed 32, stm32_seq.c:90/154), **not** `CFG_TASK_NBR`, so no existing task id shifts. New count = 21 (≤ 32). |
| `STM32_WPAN/App/app_leak_detection.c` | Added `ELEAK_HEARTBEAT_PERIOD_MS` (100 s) + `ELEAK_HEARTBEAT_BURST_DURATION_X10MS` (150 → ~1.5 s, ≥3 adv events); `leak_heartbeat_timer` (PERIODIC); registered `Leak_Heartbeat_Task`; created the timer in `APP_LEAK_Init` and **started it after `init_done = 1`** (advertising up); `Leak_Heartbeat_Cb` (timer ctx → `UTIL_SEQ_SetTask` at `CFG_SEQ_PRIO_1`) and `Leak_Heartbeat_Task` (→ `eleak_start_burst(150)`). |

### Mechanism
- A periodic LSE-backed `UTIL_TIMER` (mirrors `app_battery.c`'s timer) fires every **100 s** →
  `Leak_Heartbeat_Cb` schedules `CFG_TASK_LEAK_HEARTBEAT` → `Leak_Heartbeat_Task` calls
  `eleak_start_burst(150)` → ~1.5 s bounded burst on handle 0 → terminates (`0x12`, status `0x3C`) →
  `APP_LEAK_OnAdvTerminated` re-arms continuous (Phase 2/3a baseline).
- The burst re-loads the **current** payload, so during a live leak each heartbeat re-emits `leak=1`
  (the plan's "heartbeat re-emits the leak burst" arbitration — same handle 0, no separate set to race).

### Why 100 s / N≥3 (cadence rationale)
The hub re-stamps health `last_seen` only when it **receives** an advert inside its 5-min
`BLE_LEAK_HEARTBEAT_MS` window (gate at `app_ble_leak.c:185`, check-in at `:214`). Period 100 s gives
**3 transmit windows per 5-min hub window**, and a 1.5 s burst yields **≥3 advertising events** (each =
3 channels) against the hub's ~50 %-duty passive scan — so the probability the hub misses *every* advert
in a 5-min window is negligible even at a pessimistic single-advert miss prob `q`. Constants only;
**Phase 4 tunes period/N from the measured `q`** (only ever *down*, and only if `q` proves negligible).

## Bench verification (Phase 3a gate — continuous still on, so this is low-risk)
Build **Debug** (logs on), flash, leave idle (dry):
1. **Heartbeat fires** — `>> LEAK: ADV burst terminated (status 0x3C)` appears **every ~100 s**.
2. **No disruption** — continuous advertising/leak path unaffected; wet/dry still detected immediately;
   leak-edge burst still fires (its own `terminated` log) independent of the heartbeat cadence.
3. **Leak-active heartbeat** — hold a leak; confirm heartbeats keep firing and the leak byte stays
   advertised (buzzer alert unaffected).
4. (Optional) confirm the ~100 s cadence on a current capture — short 1.5 s burst windows.

> Note: with continuous adv ON you will **not** see offline/online behavior change here — that is expected.
> The heartbeat only becomes load-bearing in Phase 3b (continuous removed). Phase 3a just proves the timer
> + burst + terminated path at the heartbeat cadence.

## Rollback
Comment out the single `UTIL_TIMER_StartWithPeriod(&leak_heartbeat_timer, ...)` line in `APP_LEAK_Init`
(the timer is created but never armed → inert). Everything else stays harmless.

## Status
- [x] Code implemented (this commit)
- [ ] Built (Debug + Release)
- [ ] Bench-verified (items 1–3)

---
### ⛔ Next: Phase 3b is the risky one — needs explicit approval + the multi-hour soak
Phase 3b removes continuous advertising (the safety net) and cuts over to Stop1-idle-between-bursts. Per
the plan it must pass: ≥2 h no-false-offline soak, leak latency unchanged, **Stop1 re-entry after each
burst**, leak-asserted-through-soak buzzer/heartbeat concurrency, and the LR slide-switch regression. Do
not start 3b until 3a is bench-verified and you approve.
