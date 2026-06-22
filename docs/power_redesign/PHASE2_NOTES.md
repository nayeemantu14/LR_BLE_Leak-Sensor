# Phase 2 — Decoupled leak-edge burst + `ADVERTISING_SET_TERMINATED` (IMPLEMENTED)

> Scope per [GATE4_PLAN.md](GATE4_PLAN.md) Phase 2. **Continuous advertising still runs** — the burst
> is deliberately redundant here. Goal: prove the Duration-bounded enable + terminated-event path on the
> Basic-Plus stack, and **measure `q`** (single-advert miss probability) before removing the net in Phase 3b.
> No hub change. Payload byte-identical. TX power unchanged (5.6 dBm / `0x1F`).

## What changed (sensor firmware only)

| File | Change |
|---|---|
| `STM32_WPAN/App/app_leak_detection.h` | Declared `void APP_LEAK_OnAdvTerminated(uint8_t adv_handle, uint8_t status);` |
| `STM32_WPAN/App/app_leak_detection.c` | Added `ELEAK_ADV_CONTINUOUS_BASELINE` (=1) and `ELEAK_LEAK_BURST_DURATION_X10MS` (=400 → ~4 s); factored continuous enable into `eleak_enable_continuous()`; added `eleak_start_burst()`; fire a bounded burst on every leak-state change in `Leak_Process_Task`; added `APP_LEAK_OnAdvTerminated()` |
| `STM32_WPAN/App/app_ble.c` | Added `case HCI_LE_ADVERTISING_SET_TERMINATED_SUBEVT_CODE` inside `USER CODE BEGIN SUBEVENT` (regen-safe), dispatching to `APP_LEAK_OnAdvTerminated()` |

### Mechanism
- Leak edge (both onset **and** clear) → existing 50 ms debounce → `Leak_Process_Task`:
  1. flips `a_EleakAdvData[18]` and refreshes the continuous payload (unchanged behavior), then
  2. calls `eleak_start_burst(400)` → reloads payload → **silent disable** (`Enable=0`, does *not* raise
     the terminated event) → **bounded re-enable** (`Enable=1`, `Duration=400`).
- After ~4 s the controller raises `HCI_LE_ADVERTISING_SET_TERMINATED` (subevent `0x12`) → app_ble.c case →
  `APP_LEAK_OnAdvTerminated(handle=0, status)` → (Phase 2 baseline) **re-arms continuous advertising**.

Net radio effect in Phase 2: continuous advertising is effectively never off (a momentary disable/re-enable
during the burst, same interval/PHY/payload), so the burst adds reception redundancy on the safety-critical
edge while we validate the primitive.

### Arbitration / ordering note (benign in Phase 2)
If two leak edges land within one 4 s burst, or a stale burst-1 terminated event is processed after burst-2
started, the terminated handler may re-arm **continuous** instead of letting burst-2 run to its own bound.
This favors "keep advertising the leak" (the safe direction) and is harmless while continuous is the
baseline. The same handle-0 serialization is an explicit must-pass in the Phase 3b leak-asserted soak.

## Build / flash
STM32CubeIDE, **Debug** config first (UART/RTT logs on; `CFG_LPM_LEVEL=0`). The `>> LEAK: ADV burst
terminated` line confirms the terminated path. Then a **Release** build to confirm it compiles clean with
logging compiled out (`UNUSED(status)` guards the unused-param warning).

## Bench verification (Phase 2 gate)
1. **Burst visible** — wet the probe; on a sniffer (or the hub) confirm advertising continues and the
   terminated log fires ~4 s after the edge; device returns to continuous.
2. **Leak→hub latency unchanged (~0.6 s)** — wet probe → time to first `leak=1` advert received. Must match
   pre-Phase-2 behavior (continuous still carries it; burst must not regress it).
3. **No lockup** — repeat wet/dry cycles; advertising must always resume continuous after each burst;
   buzzer alert behavior unchanged.
4. **Measure `q`** — the single-advert **miss probability** against the hub's ~50 %-duty passive scan
   (window 80 / interval 160). Method: send a known number of *isolated* adv events (or count adverts in a
   burst vs adverts the hub logged) and compute miss rate. **`q` sets the heartbeat burst count `N` and
   period in Phase 3a/4.** Record it here when measured: `q = ____`.
5. **LR strap regression (quick)** — flip the LR_BUT slide switch; device still resets and re-latches
   1M↔Coded (path untouched, but re-confirm).

## Rollback
Set `ELEAK_LEAK_BURST_DURATION_X10MS` aside and remove the single `eleak_start_burst(...)` call in
`Leak_Process_Task` (the helpers + terminated handler are inert without it). Continuous advertising behavior
returns to exactly pre-Phase-2. The app_ble.c case is harmless to leave in place.

## Status
- [x] Code implemented (this commit)
- [ ] Built (Debug + Release)
- [ ] Bench-verified (items 1–5 above); `q` recorded
