# Gate 1 — Discovery: eFloStop II BLE Leak Sensor (STM32WBA) Power Redesign

**Scope:** read-only investigation of the current sensor firmware. No code changed.
**Device:** STM32WBA5M, single-core, single binary (NOT STM32WB — no coprocessor/FUS/dual-flash).
**Repo:** `C:\Work\Projects\ST Workspace\LR_BLE_Leak Sensor\BLE_HR_P2PServer`
**Method:** parallel grounded investigation + direct file verification of the load-bearing/safety-critical
paths. Every claim cites file + function + line. Items not determinable from code are marked
**[needs-bench]** or **[needs-schematic]**; framework knowledge not read from a file is **[memory]**.

> **Status legend:** ✅ code-confirmed · 🔬 needs-bench · 📐 needs-schematic · 🧠 memory (not cited)

---

## 1. BLE role & topology

**Effectively a NON-CONNECTABLE BROADCASTER with no RX path.** The CubeMX template sets the device up as
a connectable GATT peripheral (`GAP_PERIPHERAL_ROLE`, services HRS/DIS/P2P registered, bonding/MITM code
present), but the **live advertising is replaced** by a custom non-connectable advertiser, so those
services are unreachable in the field.

| Aspect | Finding | Cite |
|---|---|---|
| GAP role | `role |= GAP_PERIPHERAL_ROLE` only; `aci_gap_init(...)`; HRS/DIS/P2P registered | ✅ `app_ble.c:1343-1344, 1352-1357; 304-306` |
| Live adv path | Template fast-advertise **replaced** by custom init: `APP_BLE_Init` calls `APP_BATTERY_Init()` then `APP_LEAK_Init()`; comment *"Initialize extended advertising (replaces legacy PROC_GAP_PERIPH_ADVERTISE_START_FAST)"* | ✅ `app_ble.c:321-324` |
| Connectable path | `PROC_GAP_PERIPH_ADVERTISE_START_FAST` is invoked **only** from the disconnect handler — never at boot, and the device never connects → **dead code in the field** | ✅ `app_ble.c:420` (only caller) |
| Connectability | Non-connectable in **both** modes: `HCI_ADV_EVENT_PROP_CONNECTABLE` never set. 1M mode `adv_properties = LEGACY|SCANNABLE` (ADV_SCAN_IND); Coded mode `adv_properties = 0` (ext non-conn/non-scannable) | ✅ `app_leak_detection.c:85-97` |
| Hub consumption | Hub **passively scans** the advertising payload (no connection, no GATT read) — see §3 | ✅ hub `app_ble_leak.c` |
| RX path (config/time/OTA) | **None.** No scanning/central/observer code, no connectable window during operation. The device cannot currently receive anything. | ✅ (absence; A-stream) |

**Two PHY/range modes, latched at boot by a strap pin** `LR_BUT` (PB7): GND → Coded-PHY long-range
(extended, non-conn/non-scannable, `secondary_phy=0x03`); HIGH → 1M legacy `ADV_SCAN_IND`. A runtime edge
on PB7 triggers `NVIC_SystemReset()` to re-latch the mode. (`app_leak_detection.c:84-99, 291-295`; `main.h:89-91`)

---

## 2. BLE stack & feature support  *(decides Gate-2 feasibility)*

### Sensor (STM32WBA)
| Item | Value | Cite |
|---|---|---|
| Host middleware | STM32 WPAN **V2.9.0** (03-Feb-2026) | ✅ `Middlewares/.../Release_Notes.html:190` |
| Link layer | Synopsys DWC_ble154combo **2.00a-lca04/05**; `ll_sys` 1.0.0 | ✅ `ll_fw_config.h:1; ll_version.h:25-27` |
| Built variant | **`BLE_STACK_BASIC_PLUS_FEATURES`** in the **STM32CubeIDE** project (links Basic-Plus libs, `ble_basic_plus` LL config); `.ioc` `BLE_STACK_TYPE_BASIC_PLUS` | ✅ `STM32CubeIDE/.cproject:48,80,109; .ioc:336` |

⚠️ **Build-config inconsistency:** the **Keil/IAR/.mxproject** configs define `BLE_STACK_BASIC_FEATURES`
(a *different, smaller* variant). Only CubeIDE builds Basic-Plus. **Confirm which toolchain produces the
production binary** before relying on any Basic-Plus-only feature. (✅ `.mxproject:7,17; MDK-ARM/*.uvprojx:338; EWARM/*.ewp:239`)

### Feature matrix (compiled-in?)
| Feature | Sensor (`ble_basic_plus`) | Hub (NimBLE / ESP-IDF 5.5.1) |
|---|---|---|
| Legacy advertising | ✅ `ll_fw_config.h` | ✅ |
| **Extended advertising** | ✅ **in use** `SUPPORT_LE_EXTENDED_ADVERTISING=1` (`:77-79`); host `BLE_OPTIONS_EXTENDED_ADV` (`app_conf.h:131`) | ✅ `EXT_ADV=y, EXT_SCAN=y` (`sdkconfig:731,738`) — in use |
| **Periodic advertising (adv + sync)** | ❌ **`SUPPORT_LE_PERIODIC_ADVERTISING=0`** (`:81-83`); PAST=0, ADI=0 | ⚠️ compiled-in but **`MAX_PERIODIC_SYNCS=0`** (no capacity) (`sdkconfig:740`) |
| **PAwR** (both roles) | ❌ `SUPPORT_LE_PAWR_*=0` (`:133-139`) | ❌ `PERIODIC_ADV_WITH_RESPONSES` unset (`sdkconfig:737`) |
| Coded PHY | ✅ `SUPPORT_CSSA=1` (`:130`); S=8 **not explicitly programmed** | ✅ scans Coded |
| 2M PHY | ✅ available, unused | ✅ |

> **Gate-2 consequence (carry forward):** **periodic-advertising and PAwR are NOT feasible** without (a)
> swapping the sensor to the **Full LL library** + enabling the macros, **and** (b) rebuilding the hub with
> sync capacity. The feasible, no-new-dependency baseline is **continuous hub scan + extended/legacy adv**.
> Coded-PHY S-coding and the production toolchain are 🔬/open items.

---

## 3. Hub scan behaviour  *(ESP32-S3 NimBLE — read-only)*

| Aspect | Finding | Cite |
|---|---|---|
| API | `ble_gap_ext_disc()` extended discovery, **dual-PHY** (1M + Coded) in one session; legacy `ble_gap_disc()` fallback | ✅ hub `app_ble_leak.c:263-320` |
| Interval/window | itvl=160 / window=80 (0.625 ms) → **100 ms interval, 50 ms window ≈ 50 % duty** | ✅ `:267-276, 305-306` |
| Continuous? | `duration=0, period=0` (continuous); auto-restart on `DISC_COMPLETE` + self-heal if scan stolen | ✅ `:280-281; 246-249` |
| Passive/active | **Passive** on all paths (never requests SCAN_RSP) | ✅ `:270,276,303` |
| Dup filtering | **Disabled** (`filter_duplicates=0`) — every advert reaches the callback | ✅ `:282-284` |
| Sequence/anti-replay | **NONE.** Dedup is software **value-delta** (leak flag, battery, fw string) + a **5-min forced heartbeat**; no seq field is read | ✅ `process_leak_adv():179-187` |
| Parse | name `"eleak"` (len 5, case-insensitive), company `0x0030`, leak[1B], battery[1B], fw[3B] | ✅ name match + mfg parse |

> **Reliability implications:** (1) the hub scans only **~50 %** of the time, so a *single* advert can land
> in an off-window and be missed → redundancy (a burst of repeats) materially raises reception probability.
> (2) **No sequence counter exists**, so the hub currently **cannot detect a missed report** unless a value
> changes; liveness rests on the 5-min heartbeat only. Both are central to Cross-Cutting Requirement A.

---

## 4. Leak sense path

**Interrupt-driven, EXTI13 on PC13 ("MSense").** No polling, no ADC, no firmware power-enable for the
electrode.

| Aspect | Finding | Cite |
|---|---|---|
| Line / edges | PC13 / `GPIOC`, EXTI13, `GPIO_MODE_IT_RISING_FALLING`, `GPIO_NOPULL` (PC13 is backup-domain; internal pulls ignored — external **390 K** PCB pull-up gives dry/HIGH; water → LOW) | ✅ `main.h:86-88; main.c MX_GPIO_Init:619-622; app_leak_detection.c:20-26` |
| Flow | EXTI ISR masks EXTI13 NVIC (glitch-storm guard) → 50 ms one-shot debounce → seq task re-reads PC13 (LOW=leak/HIGH=dry) → updates adv byte 18 → re-enable EXTI13 | ✅ `app_leak_detection.c:203-289` |
| Electrode bias | No FW power-enable; probe node continuously biased by passive 390 K pull-up → standing current when **wet** is board-level | 📐 `main.c MX_GPIO_Init:603-660` (input only) |
| Wakes from idle mode? | **Yes** — EXTI wakes Stop1 (the mode in use). Standby is force-disabled precisely because WBA Standby does **not** wake from EXTI | ✅ `app_conf.h:573-594` |

---

## 5. Low-power architecture

| Aspect | Finding | Cite |
|---|---|---|
| `CFG_LPM_LEVEL` | Defined 1, **overridden by `#ifdef DEBUG`**: Debug → **0** (Run/Sleep, no Stop); Release → **2** (production) | ✅ `app_conf.h:236-247, 564-570` |
| Deepest idle mode | **Stop1.** `CFG_LPM_STANDBY_SUPPORTED=0` (Standby has no EXTI wake on WBA); `CFG_LPM_STOP2_SUPPORTED` **not defined**; `STOP1_SUPPORTED=1` | ✅ `app_conf.h:593-594, 622-627` |
| RAM retention | Stop1 retains all RAM inherently. The explicit SRAM/Radio standby-retention calls are compiled OUT (guarded by STANDBY||STOP2, both false) | ✅ `app_entry.c SystemPower_Config:386-399` |
| Wake clock | **LSE + RTC.** RADIO sleep timer clocked from LSE (per-adv-event wake); RTC (BINARY) drives `UTIL_TIMER` (debounce/battery/alert). No LPTIM. | ✅ `main.c SystemClock_Config:184-216; PeriphCommonClock:229-241; MX_RTC_Init:485-548` |
| Core between adv events | **Sleeps in Stop1 between individual adv events** (not held awake across the window). Custom USER-CODE in `UTIL_SEQ_PreIdle` calls `APP_SYS_BLE_EnterDeepSleep()` so the **radio LL deep-sleeps before** Stop1 — *"else radio clocks stay alive, idle current ~2.5 mA"* | ✅ `app_entry.c:485-544` |
| LPM blocker | Buzzer holds **Sleep** (CPU off, clocks on — not Stop1) during each ~2 ms tone pulse (Stop1 transients corrupt the magnetic indicator) | ✅ `app_alert.c:11-21, 215-223` |

> **Redesign opportunity (not decided here):** the design is at **Stop1**, not Stop2/Standby. A burst/sleep
> model would idle with **no active advertising between bursts**, opening the door to a deeper mode (Stop2
> retaining EXTI + RTC). **Whether Stop2 wakes from EXTI13 on this WBA5M silicon and retains the needed
> state is a Gate-2 / bench item.** 🔬

---

## 6. Advertising parameters (runtime, the real values)

| Parameter | Value | Cite |
|---|---|---|
| Type | Continuous (`Duration=0`, `Max_Ext_Adv_Events=0`); 1M = ADV_SCAN_IND (scannable, non-conn), Coded = ext non-conn/non-scannable | ✅ `app_leak_detection.c:102-149` |
| Interval | min 500 / max 700 × 0.625 ms = **312.5 – 437.5 ms** | ✅ `:106` |
| Channels | 37 + 38 + 39 (all primary) | ✅ `:107` |
| TX power | index **`0x1F`** — ⚠️ documented as both **"+10 dBm"** (adv config + TX-Power AD) and **"5.6 dBm"** (`app_conf.h:42`). Power table has a +10 dBm entry. **Resolve on RF bench.** | 🔬 `:112; app_conf.h:42; power_table.c:40-72` |
| Payload | fixed **23 bytes**, pushed COMPLETE; no scan-response | ✅ `:52-58, 128-133` |

> ⚠️ The `app_conf.h` `ADV_INTERVAL_MIN/MAX` macros (500/700 interpreted as **ms**) feed only the *dead*
> connectable path (`app_ble.c:942-943`) — **do not** use them for the model. The real interval is the
> 0.625 ms-unit literal in `app_leak_detection.c`.

**Advertising payload `a_EleakAdvData[23]`** (`app_leak_detection.c:52-58`):
`[0..2]` TX-Power-Level AD · `[3..9]` Complete Local Name `"eleak"` · `[10..13]` Appearance (Generic Sensor)
· `[14..19]` Mfg-Specific: len `0x08`, type `0xFF`, **Company `0x0030` (ST)**, `leak@[18]`, `batt@[19]` ·
`[20..22]` FW version `1.0.1`.

---

## 7. Battery reporting & contract  *(preserve this)*

| Aspect | Finding | Cite |
|---|---|---|
| Source | ADC4_IN3 on **PA6**, 12-bit, 814.5-cyc sampling; `BAT_Read` (PB5) enables a 2:1 divider only during the read (5 ms settle, 1 conversion, then off) | ✅ `app_battery.c:18-55, 99-141`; 📐 divider ratio = firmware constant `2.0` |
| Conversion | `Vbat = raw/4095 × 3.3 × 2.0`; 6-point CR2032 piecewise curve (2.0 V=0 % … 3.0 V=100 %) → uint8 | ✅ `app_battery.c:148, 200-226` |
| **Contract** | **uint8 0–100** written to adv **`[19]`** via `APP_LEAK_Update_Battery()`, re-pushed live. **Preserve as-is.** | ✅ `app_battery.c:153; app_leak_detection.c:297-302` |
| Cadence | adaptive: **1 h** (≥20 %) / **1 min** (10–19 %) / **10 s** (<10 %) | ✅ `app_battery.c:13-15, 233-247` |
| Rail | **TPS63900 buck-boost** (1.8 V min in) per code comment — affects usable mAh & Iq | ✅ `app_battery.c:189` (comment) |

---

## 8. Payload identity / security

**Plaintext, no security, no sequence number.** The advert carries name + ST mfg data (`leak`, `battery`,
`fw`) only. **No encryption, no authentication, no rolling/sequence counter** — anywhere on sensor or hub.
(✅ `app_leak_detection.c:52-58`; hub `process_leak_adv` has no seq field.) This is the gap Cross-Cutting
Requirement A (dedup + gap detection + optional authenticated alarm) must close.

---

## Current power model

**Inputs (code-confirmed):** continuous advertising every **~375 ms avg** on **3 channels** at TX index
`0x1F`, 23-byte payload; Stop1 between events with radio-LL deep-sleep; battery ADC ~once/hour
(negligible); buzzer only during alerts (leak: ~40 ms ON / 8 s while wet; idle: 0).

**Structure** (average current):
```
I_avg ≈ I_stop1_leak  +  N_adv × Q_adv  +  I_batt_adc(~negligible)  +  I_alert(0 when dry)
        └ Stop1 sleep    └ 1000/375 ≈ 2.67 adv-events/s, each Q_adv coulombs (3× TX @0x1F + ramp,
          leakage (µA)     + in 1M mode a SCAN_REQ listen window after each ADV_SCAN_IND)
```

**The dominant terms are (a) the radio energy of continuous +10 dBm advertising and (b) Stop1 leakage.**
Everything else is negligible in the dry/idle state.

**Numbers that must be measured/derived before the model is quantitative (do not guess):**
| Quantity | Source needed | Status |
|---|---|---|
| `Q_adv` (charge per 3-channel adv event @ `0x1F`, incl. ramp + 1M scan-listen) | bench (current probe) or WBA5x radio-current datasheet table | 🔬 |
| Stop1 sleep leakage (µA) on this WBA5M | datasheet typ + bench | 🔬 |
| True TX dBm of `0x1F` (+10 vs +5.6) | RF bench / ST power table decode | 🔬 |
| Radio-LL deep-sleep actually engaging (else ~2.5 mA) | bench (the code asserts it; verify) | 🔬 |
| TPS63900 Iq + efficiency at this load profile | TI datasheet + bench | 🔬 |
| CR2032 usable mAh under ~375 ms pulse load (vs 225 mAh nominal) | bench / cell derating (see §D pulse work) | 🔬 |

**Order-of-magnitude bracket (explicitly provisional):**
- **Failure mode** (radio LL not deep-sleeping, per the code's own ~2.5 mA note): 225 mAh / 2.5 mA ≈ **~4 days**.
- **Working-as-intended** (deep-sleep engaged; advertising energy + Stop1 leakage dominate): plausibly tens
  of µA average → **months**, but the spread is wide and hinges entirely on `Q_adv`, the true TX dBm, and
  Stop1 leakage — **all 🔬**. *A single bench average-current measurement at idle would collapse this
  bracket and anchor the whole redesign; it is the highest-value first measurement.*

---

## Headline observations for the redesign (no decisions made here)

1. **Continuous +10 dBm advertising every ~375 ms is the dominant draw** and the single biggest lever
   (cadence ↓, TX power ↓, burst-then-sleep).
2. **Leak is not bursted** — a leak just flips adv byte `[18]` into the continuous stream, so worst-case
   leak→hub latency ≈ debounce(50 ms) + adv interval(≤437 ms) + hub 50 %-duty scan(≤100 ms) ≈ **~0.6 s**.
   A redesign that adds sleep between bursts must **decouple the leak path** so this does not regress.
3. **No sequence counter** → gaps are invisible today; adding one (Req A) is cheap and high-value.
4. **Periodic-adv / PAwR are not compiled in** (sensor Basic-Plus LL; hub sync capacity 0) → bias Gate 2
   toward **continuous-scan + redundant burst + sequence numbers (+ optional acked alarm)**; treat
   periodic/PAwR as feasible only via a Full-LL swap + hub rebuild, justified only if it measurably beats
   the simple scheme.
5. **Stop1 today, not Stop2** — a burst/sleep model may unlock a deeper sleep, pending an EXTI-wake-from-Stop2
   feasibility check.
6. **Preserve:** uint8 0–100 battery `[19]`, company `0x0030`, name `"eleak"`, leak `[18]` — the hub-facing
   contract.

## Open items to confirm (carried into Gate 2/3)
- 🔬 Idle average current (anchors everything) · per-event `Q_adv` · Stop1 leakage · true TX dBm of `0x1F`
  · radio-LL deep-sleep engaging · TPS63900 Iq/efficiency · CR2032 usable mAh under pulse.
- 📐 Leak electrode wiring + 390 K value + standing wet current · battery divider ratio (fw assumes 2.0).
- ❓ **Production toolchain** (CubeIDE Basic-Plus vs Keil/IAR Basic) · Coded-PHY S-coding actually on air.

---
**Gate 1 complete — read-only, no firmware changed. Awaiting your review before Gate 2.**
