# Gate 4 — Phased Plan: STM32WBA Leak-Sensor Power Management

> **Sensor firmware only. No hub changes this sprint. No new dependencies. Plan only — do not implement
> until approved.** Grounded in [GATE1_DISCOVERY.md](GATE1_DISCOVERY.md) + [GATE2_RESEARCH.md](GATE2_RESEARCH.md);
> revised after a 3-reviewer adversarial pass (safety/ordering · firmware-accuracy · scope/hub).

## Context
The sensor today **advertises continuously** every ~375 ms at 5.6 dBm and never sleeps the schedule
([app_leak_detection.c:102-149](file)); a leak just flips a byte in that stream — ~230,000 radio events/day
for almost no information, the dominant battery drain. This sprint replaces continuous advertising with an
**event-driven burst** model that idles in **Stop1** (the WBA floor) between reports, **without regressing
leak responsiveness** (decoupled leak path, fired immediately on detection).

## Locked decisions (Gate 3)
- **Battery ~2 years** on ~225 mAh CR2032 → average current **≤ ~12.8 µA**.
- **Leak latency ~0.6 s** preserved.
- **TX power stays 5.6 dBm** (`0x1F`). **Keep the 1M/Coded boot strap** (default 1M; Coded escape hatch).
- **Firmware-only**; reservoir cap = next-PCB-rev doc recommendation.
- **Deferred:** leak authentication; rolling sequence counter + hub gap-detection.
- **Sleep = Stop1** (no Stop2 on WBA; Standby no EXTI wake).

## ⚠️ Heartbeat cadence — bound by the hub's **5-min** constant, not 10-min (review fix)
The hub stamps health `last_seen` only when an advert passes its gate `if (!data_changed && !heartbeat_due)
return;` ([app_ble_leak.c:185](file)), with the check-in at [:214](file). For a quiescent sensor (battery byte
unchanged up to 1 h), `data_changed` is false, so `last_seen` is refreshed **only** when the hub's own
`heartbeat_due` fires at **`BLE_LEAK_HEARTBEAT_MS = 5 min`** ([app_ble_leak.c:37](file)) — keyed off the last
*received* advert. The 10-min `HEALTH_BLE_LEAK_TIMEOUT_MS` offline is then a **2-window budget** on top.
**Binding rule:** the sensor must land a *received* advert well inside each 5-min window. → **heartbeat period
~90–120 s with a burst of N≥2–3 events** (not a single event), giving ≥2–3 independent reception chances per
5-min window even at the bench-measured worst-case miss probability `q` (against the hub's ~50 %-duty passive
scan, window 80 / itvl 160, [app_ble_leak.c:267-276](file)). The hub's 5-min gate is a **frozen interface
constant** this sprint — the sensor adapts to it.

## Why ~2 years becomes a single bench question
Event-driven advertising collapses the radio term: heartbeat ~720–960 short bursts/day + rare leak bursts ≈
**<0.1 µA time-averaged**; ADC battery read (1/h) negligible. So **average current ≈ Stop1 leakage**, and the
2-year target reduces to *“is Stop1 leakage (MCU + LSE/RTC + TPS63900 Iq) ≤ ~12.8 µA?”* (datasheet Stop1
ceiling ~16 µA; an LSE/RTC-only build should sit lower). **Phase 0 measures it.** This is the brief's goal —
drive average current to the Stop-mode floor, then stop.

---

## Target design shape
- **Idle:** Stop1, radio-LL deep-sleep, EXTI13 (leak) + RTC (heartbeat) armed, **no advertising**.
- **Heartbeat (~90–120 s):** RTC wake → latch current battery/leak bytes (`aci_gap_adv_set_adv_data`) →
  **re-issue `aci_gap_adv_set_enable(1,1,&set)`** for a **bounded burst of N≥2–3 events** → set terminates
  (`ADVERTISING_SET_TERMINATED`) → Stop1.
- **Leak edge (async):** EXTI13 → 50 ms debounce → flip leak byte → **immediate bounded burst (~3–4 s)** →
  Stop1. Decoupled from the heartbeat schedule; latency unchanged (~0.6 s).
- **Burst-bound primitive (review fix):** `Adv_Set_t` exposes **both** `Duration` (uint16, ×10 ms) and
  `Max_Extended_Advertising_Events` (uint8) ([ble_types.h:48-73]). On the **legacy 1M energy path use
  `Duration`** (Max_Extended_Advertising_Events is extended-adv-only); on the **Coded extended strap** either
  works. Both raise `HCI_LE_ADVERTISING_SET_TERMINATED` (subevent `0x12`).
- **Terminated handler (review fix):** new `case HCI_LE_ADVERTISING_SET_TERMINATED_SUBEVT_CODE:` inside the
  `USER CODE BEGIN SUBEVENT` block ([app_ble.c:578-580](file)) — survives CubeMX regen; just lets the loop
  return to idle/Stop1.
- **PHY/type:** 1M **`ADV_SCAN_IND` (scannable, kept as-is** — `adv_properties = LEGACY | SCANNABLE`, decision
  2026-06-22, preserves a future hub `SCAN_REQ` path); non-connectable in both modes; Coded strap
  (`adv_properties=0`) unchanged. **Payload byte-identical** to today. *(Bursts are simply N scannable adv
  events; the brief scan-listen RX only occurs during a burst, not in the long Stop1 idle.)*
- **Leak/heartbeat arbitration (review fix):** advertising set handle 0 is shared. While `leak_active`, the
  leak byte is always current in the payload, and heartbeat ticks **re-emit the (leak=1) burst** (the hub
  dedups by value) rather than racing a separate set — so a heartbeat never cancels an in-flight leak advert.
  The buzzer's `UTIL_LPM_SLEEP` hold during ~200 ms tone pulses ([app_alert.c:17-18](file)) coexists with
  Stop1 in the gaps; verified in the Phase-3b leak-soak test.

---

## Phases (independently testable, independently revertible, sequenced)

### Phase 0 — Bench baseline (MEASURE FIRST, no code)
- Measure idle **average current** of the current continuous-adv Release build + a Stop1-only reference
  (advertising disabled) to bound the floor. Anchors the model and the 2-yr feasibility.
- **Verify:** current-probe reading; record continuous-adv avg vs Stop1 floor. **Gate:** if already
  Stop1-floor-dominated (unlikely), re-scope.

### Phase 1 — DROPPED (decision 2026-06-22): keep 1M `ADV_SCAN_IND` (scannable)
We deliberately **retain the scannable 1M advertising** (`adv_properties = HCI_ADV_EVENT_PROP_LEGACY |
HCI_ADV_EVENT_PROP_SCANNABLE`, unchanged from today, [app_leak_detection.c:95-97](file)) to keep a future
**hub-side `SCAN_REQ` / scan-response** path open. We forgo the per-packet RX-window energy saving; in the
event-driven model that cost is small (it occurs only during bursts, not continuously). **Note for the future
sprint:** keeping scannable preserves the *capability* only — the sensor currently sets **no scan-response
data**, so an actual active-scan feature must (a) define+populate the sensor scan-response payload and (b)
switch the hub to active scan; and it would be **1M-only** (the Coded strap is non-scannable extended adv).
No code change in this phase.

### Phase 2 — Decoupled leak-onset burst + measure `q` (continuous adv still running)
- **Change:** in `Leak_Process_Task` ([app_leak_detection.c:259-289](file)), on a dry→leak edge fire a
  **bounded burst (~3–4 s)** (1M: `Duration`) in addition to the byte flip; add the `ADVERTISING_SET_TERMINATED`
  handler. Redundant at this phase (continuous adv still carries the leak) — that's the point: prove + measure
  before removing the net.
- **Verify:** wet probe → sniffer/hub sees the bounded burst; **leak→hub latency still ~0.6 s**; burst
  terminates and returns to idle. **Measure `q`** (single-advert miss probability) here against the hub's
  50 %-duty scan — this sets heartbeat N and period for Phase 3.
- **Rollback:** remove the burst call. **Independent:** yes (additive).

### Phase 3a — Periodic heartbeat burst timer (continuous adv STILL running)
- **Change:** add a **~90–120 s periodic LSE-backed `UTIL_TIMER`** (same pattern as `app_battery.c`'s timer;
  use a reserved `CFG_Task` slot — do **not** shift `CFG_TASK_NBR`) whose sequencer task latches bytes and
  emits a **bounded N≥2–3-event heartbeat burst**. Continuous advertising is still enabled, so this is
  additive — it isolates the heartbeat mechanism from the Stop1-cutover risk.
- **Verify:** `ADVERTISING_SET_TERMINATED` fires each heartbeat; the burst is bounded; no disruption to the
  continuous stream or leak path.
- **Rollback:** disable the heartbeat timer. **Independent:** yes (additive).

### Phase 3b — Stop continuous advertising (the battery win + Stop1 cutover)
- **Change:** remove the continuous enable (`Duration=0`, [app_leak_detection.c:144-149](file)); advertising
  now happens **only** via the Phase-3a heartbeat bursts and the Phase-2 leak bursts. Fire an **immediate
  heartbeat burst at the end of `APP_LEAK_Init`** (and on any battery/leak byte change between ticks) so boot
  state + fresh battery are advertised promptly, not after a full period. Confirm radio-LL deep-sleep + RTC/EXTI
  wake + Stop1 re-entry work with advertising fully stopped.
- **Verify (the big soak):** multi-hour run — **no `device_offline` and no false `device_recovered` toggle
  over ≥2 h**; wet probe at random → leak seen within latency; **bench average current at/near the Stop1
  floor** → project life (≥2 yr ⇔ ≤12.8 µA); confirm MCU re-enters Stop1 after each burst (not stuck awake).
  **Leak-asserted soak (review fix):** hold a leak for the full soak and verify (1) buzzer keeps sounding,
  (2) the leak byte stays advertised at ≥ heartbeat cadence, (3) heartbeat + leak bursts serialize on adv
  handle 0 without cancelling each other, (4) Stop1 is still reached in the inter-burst/inter-tone gaps.
  **LR strap (PHY select) regression:** with the device idle in a long Stop1 stretch between bursts, flip
  the LR_BUT slide switch and confirm EXTI7 still wakes Stop1 → debounce → `NVIC_SystemReset` → reboot
  re-latches 1M↔Coded. The strap path ([app_leak_detection.c:84-94, 221-230, 291-295](file); EXTI7 at
  [main.c:624-628, 638-639, 657](file)) is untouched by this sprint, but the longer Stop1 stretches make this
  an explicit must-pass rather than inherited behavior.
- **Rollback:** re-enable continuous (`Duration=0`) — restores Phase-3a behavior exactly (heartbeat timer
  stays harmless). **Independent:** yes (Phase-3a heartbeat + Phase-2 leak burst already proven).

### Phase 4 — Tune + finalize
- Set heartbeat period (~90–120 s) and burst **N** from the measured `q` so steady-state reception meets the
  chosen target within the 5-min hub window; tune N **down** only if `q` proved negligible. Constants only.
- **Verify:** final current measurement + reception soak (no false offline ≥ a few hours; leak reception 100 %
  across trials); record the 2-yr projection.
- **Rollback:** revert constants to Phase-3b values.

### Doc deliverable (no code) — CR2032 reservoir cap, next PCB rev
Document Stream-D (cell/VIN-side 47 µF X5R/X7R, 100 µF cold; TPS63900 input current-limit 10–25 mA) with the
cap-sizing math. Not implemented this sprint.

---

## Hub interface — compatibility statement (FROZEN; scoped to the leak adv/scan interface only)
**No hub change required this sprint.** Contract preserved exactly: payload byte-identical (name `eleak`,
company `0x0030`, leak `[18]`, battery uint8 `[19]`, fw `[20-22]`). Behavioral notes for the hub team (no
action):
- The sensor now advertises in **bursts** (heartbeat ~90–120 s + leak edge), not continuously. **The binding
  hub constant is the 5-min `BLE_LEAK_HEARTBEAT_MS`** (re-stamps `last_seen`); the 10-min offline is the
  2-window budget. The sensor cadence is chosen to sit comfortably under 5 min with burst redundancy.
- **RSSI-based health** ([health_engine.c:125-135]) now samples at the heartbeat cadence (sparser) rather than
  continuously — confirm no spurious WARNING flaps during the Phase-4 soak.
- Existing `filter_duplicates=0`, value-delta dedup, and the 10-min offline already tolerate a bursty sender.
- When the deferred sequence-counter ships, *that* becomes an additive payload change + a hub gap-detection spec.

## Scope discipline / explicitly dropped
❌ Stop2/Standby · ❌ periodic-adv / PAwR · ❌ acked retransmit / sensor RX · ❌ TX-power change (kept 5.6 dBm)
· ❌ Coded removal (strap kept) · ❌ leak auth + sequence counter (future sprint) · ❌ pulse-shaping state
machines · ❌ hub code changes. **Stop rule:** once Phase 3b shows average current at the Stop1 floor, stop.

## Assumptions & bench/schematic confirm items
- 🔬 Stop1 idle leakage of the *eleak* PCB (the 2-yr gate) · per-burst TX energy at 5.6 dBm/1M · **`q`
  single-advert miss probability (Phase 2, sets N)** · `Duration`-bounded burst + `ADVERTISING_SET_TERMINATED`
  behave on the Basic-Plus stack · radio-LL deep-sleep + RTC/EXTI wake + Stop1 re-entry with adv stopped ·
  `aci_gap_adv_set_adv_data` allowed while stopped · leak→hub latency unchanged · ≥2 h no-false-offline soak ·
  leak-asserted-through-soak buzzer/heartbeat concurrency.
- 📐 Reservoir-cap placement + VIN droop under a burst (next PCB rev).

---
**Gate 4 plan (revised post-review) — no firmware changed. Awaiting approval before any implementation.**
