# Phase 3b — Stop1 cutover (continuous adv removed) — IMPLEMENTED + REVIEWED

> Scope per [GATE4_PLAN.md](GATE4_PLAN.md) Phase 3b. **This is the safety-critical cutover**: continuous
> advertising is removed; advertising now happens only in bounded bursts and the MCU idles in **Stop1**
> between them. Sensor firmware only. Payload byte-identical. TX power unchanged (5.6 dBm). No hub change.
>
> Two adversarial passes were run before flashing: a 4-reviewer safety review (verdict **go-with-fixes**,
> 2 must-fix + 2 should-fix) and a 2-reviewer fix-verification (verdict **fixes-sound**, 1 low + 1 info,
> both now closed). **Bench-flash + the soak below are still required before this is trusted.**

## What changed (`STM32_WPAN/App/app_leak_detection.c`, `Core/Inc/app_conf.h`)

### The cutover
- `#define ELEAK_ADV_CONTINUOUS_BASELINE 0` (was 1) — the single switch. Effect:
  - boot continuous-enable is `#if`-gated **off** (adv set stays configured but disabled);
  - `APP_LEAK_OnAdvTerminated` no longer re-arms continuous → returns to idle → sequencer idle path → **Stop1**;
  - an **immediate boot burst** at end of `APP_LEAK_Init` (`leak_state ? 4 s : 2.5 s`) so boot/wet-at-boot
    state advertises promptly; the async first battery read lands in-place during it.
- Battery-% updates ride the next heartbeat (≤ heartbeat period; non-critical) — no separate battery burst.
- `eleak_enable_continuous()` proto+def are `#if`-gated so the rollback config (`=1`) stays warning-clean.

### Review fixes folded in (all in this cutover)
1. **Burst enable is retried, not fire-and-forget** (must-fix). `eleak_attempt_burst()` checks the
   `aci_gap_adv_set_enable` return; on failure it retries up to `ELEAK_BURST_MAX_RETRIES` (3) on a 100 ms
   one-shot `burst_retry_timer` (`CFG_TASK_BURST_RETRY`). 1 initial + 3 retries = 4 bounded attempts, then
   gives up (heartbeat re-sample is the slower backstop). Post-cutover this enable is the **only** thing
   that puts a `leak=1` byte on the air, so a silent failure was a missed leak.
2. **Heartbeat re-reads the pin** (must-fix). New shared `eleak_reconcile_leak_state()` (read MSense →
   update `leak_state`/payload/alert/cadence → return changed?) is called from the EXTI path, **every
   heartbeat** (periodic ground-truth re-sample), and once post-init. Closes the lost-edge hole where a
   single dropped dry→wet edge would advertise `leak=0` forever; self-heals **both** edge directions.
3. **Heartbeat burst 150→250 ×10 ms** (should-fix) → ≥6 advertising events, so one or two missed bursts
   cannot reach the 10-min offline timeout (see math below).
4. **Adaptive heartbeat cadence** (should-fix): `ELEAK_HEARTBEAT_PERIOD_MS` = 100 s when dry,
   `ELEAK_HEARTBEAT_LEAK_PERIOD_MS` = 15 s while leaking (`eleak_set_heartbeat_period`, restart only on
   change). Bounds the hub-reboot-mid-leak re-learn gap to ≤15 s.

### Fix-verification findings (closed)
- **(low)** stale `Burst_Retry_Task` after a successful interleaving burst → one benign identical re-emit;
  documented in-code (counter is 0 there, cannot extend the budget).
- **(info, safety-relevant)** an edge in the reconcile-read→flag-clear window was recovered only at the
  heartbeat → now `Leak_Process_Task` re-arms the debounce if the settled level disagrees with `leak_state`,
  catching it in ~50 ms (self-terminating, debounce-bounded against a bouncing pin).

## ⚠️ Corrected no-false-offline math (review fix — was wrong in earlier notes)
The hub re-stamps `last_seen` only when a received advert passes `if (!data_changed && !heartbeat_due)
return;`, and for a quiescent (battery-unchanged) sensor `heartbeat_due` opens at `last_event + 300 s`
(`BLE_LEAK_HEARTBEAT_MS`), **uncorrelated** with the sensor's burst phase. So the worst-case quiescent
restamp interval is **300 s + up to one heartbeat period**, not 300 s:

```
worst-case restamp ≈ BLE_LEAK_HEARTBEAT_MS (300 s) + ELEAK_HEARTBEAT_PERIOD_MS (100 s) = 400 s
offline budget     = HEALTH_BLE_LEAK_TIMEOUT_MS (600 s)
slack              = 600 − 400 = 200 s = exactly two 100 s heartbeats
```

With the burst now ≥6 events, the per-burst miss probability is ≈ q⁶ and "two consecutive misses" ≈ q¹²,
so a false `device_offline` of a live, dry sensor is extremely unlikely — but it is **not impossible**
until confirmed against the **measured q (Phase 2 / Phase 4)**. Options if Phase 4 shows it's marginal
(all sensor-side, hub untouched, except the last): shorten `ELEAK_HEARTBEAT_PERIOD_MS` to 60–75 s,
lengthen the heartbeat burst further, or (hub change, deferred) raise `HEALTH_BLE_LEAK_TIMEOUT_MS` to
~900 s.

## Bench soak checklist (MUST pass before this is trusted; assistant cannot run hardware)
Build **Release** for the real power numbers; **Debug** first to read the logs.

1. **Stop1 re-entry (THE gate).** After each burst (boot, heartbeat, leak edge) the device must return to
   Stop1. In Release, average current should fall to/near the Stop1 floor between bursts (long flat
   stretches). In Debug you'll see the continuous ~375 ms adv spikes **gone** — replaced by a short
   ~2.5 s spike cluster every 100 s (heartbeat) + a `>> LEAK: ADV burst terminated (status 0x3C)` each.
2. **No false offline / no false recovered ≥ 2 h** (the big soak), sensor dry, watching the hub. Confirm
   no `device_offline` / `device_recovered` flap.
3. **Leak onset latency ~0.6 s** unchanged — wet probe → time to hub `leak_detected`.
4. **Leak-asserted soak.** Hold a leak through the soak: buzzer keeps sounding; leak byte re-advertised at
   the **15 s** leak cadence; heartbeat + leak bursts serialize on adv handle 0 without cancelling; Stop1
   still reached in inter-burst/inter-tone gaps.
5. **Wet-at-boot** (probe wet before power-up): boot burst is the 4 s leak burst; hub sees `leak=1` promptly.
6. **Lost-edge self-heal** (hard to force; optional): verify a heartbeat re-sample reports a sustained-wet
   pin even with no fresh edge.
7. **LR slide-switch regression** during a long Stop1 stretch: flip LR_BUT → EXTI7 wakes Stop1 →
   `NVIC_SystemReset` → re-latches 1M↔Coded.
8. **Burst-retry instrumentation:** confirm `>> LEAK: burst enable FAILED ...` does **not** appear in normal
   operation (it's the safety-net; its presence is a regression signal).
9. **Average current → 2-yr projection:** record Release idle average; ≥2 yr ⇔ ≤ ~12.8 µA (Stop1-leakage
   dominated). This is the Phase-0 anchor finally measured in the burst-only build.

## Rollback
Set `ELEAK_ADV_CONTINUOUS_BASELINE` back to **1** → continuous advertising restored exactly (Phase 3a
behavior); the heartbeat/retry/reconcile additions stay harmless. One-line, fully reversible.

## Status
- [x] Code implemented + adversarially reviewed (go-with-fixes) + fixes verified (fixes-sound)
- [ ] Built (Debug + Release)
- [ ] Soak checklist 1–9 passed; 2-yr projection recorded
- [ ] Phase 4: finalize heartbeat period / burst N / (maybe) hub timeout from measured q
