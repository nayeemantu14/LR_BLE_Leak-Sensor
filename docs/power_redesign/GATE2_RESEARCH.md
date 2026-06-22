# Gate 2 — Research Synthesis: STM32WBA Leak-Sensor Power Redesign

**Method:** five parallel research streams (A–E, web-grounded, cited) + an adversarial reconciler.
This document is a **reconciliation, not a concatenation** — it resolves the cross-stream conflicts and
converges on **one** recommended architecture, grounded in the Gate 1 stack reality (production = CubeIDE
Basic-Plus; periodic-adv/PAwR compiled out; non-connectable broadcaster; Stop1 floor; hub = mains-powered
ESP32-S3 NimBLE, ~50% scan duty, no sequence handling).

> **Status tags:** `spec` (BT Core) · `datasheet` · `app-note` · `bench` (must measure) · `memory` (recalled).

---

## 0. One-paragraph recommendation

Keep the device a **pure non-connectable RX-less broadcaster on the compiled-in Basic-Plus stack** — **no
Full-LL swap, no hub rebuild, no new libraries.** Replace today's **continuous** 5.6 dBm advertising with an
**event-driven burst** model that idles in **Stop1** (the WBA floor) between reports: a short heartbeat
burst every ~5 min, and an **immediate, decoupled N-burst on each leak edge** so leak latency never
regresses. Make each report carry a **rolling sequence counter + event-type** so the always-listening hub
can **dedup burst repeats and detect missed reports** (the hole that exists today). Lowest-energy radio
config = **1 M PHY, non-connectable *non-scannable*, 3 primary channels, minimal payload, TX power targeted
to 0 dBm** (5.6 dBm fallback; Coded PHY kept only as a long-range escape hatch). Harden the CR2032 with a
**cell-side reservoir cap**. Everything else (acked retransmit, periodic-adv, PAwR, Standby, payload
encryption, pulse-shaping state machines) is **rejected as infeasible or over-engineering**.

---

## 1. Cross-stream conflicts — resolved

| # | Conflict | Resolution |
|---|---|---|
| 1 | **Stop2 exists?** (B vs Gate 1) | **B is correct: WBA5x has NO Stop2** (modes = Run/Sleep/Stop0/Stop1/Standby). My Gate 1 "Stop2 not enabled" was an STM32WB-valve carryover. **Stop1 is the deepest mode that wakes from EXTI — and it's already in use.** The brief's "sleep in Stop2" default → **Stop1**. `datasheet`/`bench`-confirm vs RM0493. |
| 2 | **Continuous vs event-driven adv** (A vs B) — *the crux* | **Adopt event-driven (A).** B is right we can't reach a *deeper* sleep (Stop1 is the floor) and right to reject Standby; but A is right that the **radio term of continuous advertising (~2.67 events/s)** is wasted energy. B's "already at the floor" rested on a **~5–7 µA figure measured on the valve (STM32WB, a different chip in WB-Stop2)** — it does **not** transfer to this WBA sensor. First-principles, continuous adv likely dominates Stop1 leakage by several×. Event-driven bursting is firmware-only, free, and *directly serves the brief's goal* (drive avg current to the Stop-mode floor). **Gating validation = one idle-average-current bench measurement** (§7). |
| 3 | **Leak-burst count N** (A: 3–4 vs C: 10–12) | Same `P=1−q^N` model, different target. For a **safety leak-onset with no ACK**, bias to the conservative end: **N≈10–12 three-channel events over ~3–4 s**, *tunable*, with the **sequence counter converting any residual miss into a detected gap**. Exact N derives from the hub's **measured single-advert miss probability q** (bench) and the **target reception probability** (a Gate 3 decision). |
| 4 | **TX power** (A: 0 dBm vs C/B compute at 5.6 dBm) | Treat as a **coupled energy+reliability** decision. **Target 0 dBm** (large indoor margin with the hub's continuous high-sensitivity scan) but **bench-measure q at 0 vs 5.6 dBm before committing**; keep **5.6 dBm as fallback**. Do not optimise power in isolation. |
| 5 | **Scannable vs non-scannable** (A vs C/E) | Both partly right. Gate 1 confirms the **1 M mode IS `ADV_SCAN_IND` (scannable)** → it holds an RX listen window after each PDU for a `SCAN_REQ` that the **passive hub never sends**. Switching 1 M to **`ADV_NONCONN_IND` (non-scannable) deletes that wasted RX energy — a free win.** (Coded mode is already non-scannable.) |
| 6 | **Seq counter width/placement** (C: uint8 vs E: uint16) | **One counter: `uint16` LE** (E) — avoids C's uint8 wrap-guard for 1 extra byte; additive in spare mfg-data bytes; **per-logical-report** semantics (same seq across a burst's repeats). |
| 7 | **Authenticated 20-byte leak frame vs hub COMPLETE-only drop** (E vs hub code) | Real coupling: the hub **drops EXT reports with `data_status != COMPLETE`** (`app_ble_leak.c:237`). Any auth frame **must stay within one un-chained PDU** and be **bench-confirmed COMPLETE** (esp. on Coded). Auth (Tier 1) is **optional/deferred**; Tier 0 (seq+event-type, ~5 B) fits the legacy 31-B budget and ships first. |

---

## 2. Delivery / synchronisation tiers — ranked

| Tier | Reception / no-loss mechanism | Sensor power cost | Feasibility vs Gate 1 | Verdict |
|---|---|---|---|---|
| **(i) Continuous hub scan + redundant burst + rolling seq + hub gap-detect (no sensor RX)** | Burst redundancy raises capture vs 50 %-duty scan; **seq counter turns residual misses into *detected* gaps** | **~Zero standing.** Leak-edge burst = a few extra TX events (rare) | ✅ **Compiled-in today.** No swap/rebuild/re-cert | **RECOMMENDED** for both heartbeat (seq) and leak (seq + N-burst) |
| (ii) Acked retransmit for leak only | Sensor retransmits until ack | **High — adds an RX path** (connectable window or ack-beacon scan) the sensor doesn't have | Buildable but **breaks the RX-less broadcaster model** | ❌ Reject — hub already always-listening; not worth sensor RX |
| (iii) Extended advertising (status quo) | Liveness only | Current baseline | In use | ◑ Keep as the *carrier*, but it gives **no miss-detection** alone → add seq (i) |
| (iv) Periodic adv + scanner sync | Deterministic windows | Adds advertiser timing + guard-band; **net not lower** than (i) | ❌ **Sensor `SUPPORT_LE_PERIODIC_ADVERTISING=0`** → Full-LL swap + **re-cert**; hub `MAX_PERIODIC_SYNCS=0` → rebuild | ❌ Reject — wrong tool for once-per-report broadcast |
| (v) PAwR | Bidirectional response slots | Requires sensor RX even in principle | ❌ **Impossible:** ST: *"PAwR cannot be supported on WBA5"*; ESP32-S3 NimBLE has none | ❌ Hard reject |

**Recommendation:** **heartbeat = (i) seq-only fire-and-forget**, **leak alarm = (i) seq + N-burst on the edge.**

---

## 3. Sleep + advertising options — compared

| Option | Idle current | Wakes from EXTI? | Reliability/latency | Verdict |
|---|---|---|---|---|
| **Stop1 + EVENT-DRIVEN bursts** (recommended) | Stop1 floor between bursts; **radio term collapses ~100s×** vs continuous | ✅ (Stop1) | Leak burst fired immediately on debounce → ~0.6 s worst case preserved | ✅ **RECOMMENDED** |
| Stop1 + continuous adv (status quo) | Stop1 floor **+ continuous radio term** (the waste) | ✅ | Liveness ok; no redundancy/seq | ◑ Baseline — improve |
| Standby + periodic advertiser | ~1–2.5 µA but **full reset on wake** + re-init energy + reset-loop risk | ❌ **Standby does NOT wake from EXTI on WBA** | **Disqualifies the leak path** | ❌ Reject (B) |

> **WBA5x sleep ladder (corrected):** Run · Sleep · **Stop0 · Stop1 (floor, EXTI-wake)** · Standby (no EXTI).
> No Stop2. Keep **LSE + RTC** (heartbeat) and the **LSE radio sleep timer**; radio-LL deep-sleep before
> Stop1 (already hand-rolled). `datasheet`/`bench`.

---

## 4. Converged architecture (sensor-side concrete)

All within compiled-in Basic-Plus; preserves the hub contract (`eleak`, company `0x0030`, leak byte,
battery uint8 0–100).

1. **Advertising lifecycle — event-driven (the battery win).** Idle in **Stop1**; advertise only on a
   *report*: **(a) heartbeat** = one 3-channel event every ~5 min (matches the hub's existing 5-min
   heartbeat contract), carrying seq + battery; **(b) leak edge** = an immediate **N≈10–12-event burst over
   ~3–4 s**, fired the instant the 50 ms EXTI debounce completes — **decoupled from the heartbeat schedule**
   so latency never regresses; **(c) battery-delta** = one event when % crosses a threshold. Stop continuous
   advertising between reports.
2. **Lowest-energy radio config (A).** **1 M PHY** only; **non-connectable, non-scannable** (`ADV_NONCONN_IND`
   — deletes the wasted scan-RX window); **3 primary channels** per event; **minimal payload**; **TX power
   target 0 dBm** (5.6 dBm fallback; **Coded PHY = documented long-range escape hatch**, ~8× energy, only if
   a site needs it).
3. **Reliability (C tier i, no sensor RX).** Rolling **uint16 seq** per logical report + redundant leak
   burst. Hub dedups repeats and detects gaps (spec handoff §5). Reject acked-retransmit / periodic / PAwR.
4. **Payload & protocol (E).** **Tier 0 now** (ship first): additive mfg-data fields — `proto_ver`,
   `event_type` {heartbeat, leak_alarm, leak_clear, battery, power_up}, `uint16 seq`, `flags` — old hub
   ignores trailing bytes (forward-compatible). **Tier 1 optional (Gate 3 decision):** **AES-CMAC-64
   authenticated leak frames only** (HW-AES, computed on the rare edge; heartbeats stay cheap/unauthenticated;
   frame must stay one un-chained PDU). **Tier 2 (future, not now):** connect-on-demand RX window for
   config/OTA — feasibility on Basic-Plus connectable path noted, unconfirmed.
5. **Sleep (B).** **Stop1** floor; reject Standby; LSE+RTC + radio sleep timer; EXTI13 leak wake + RTC
   heartbeat wake; radio-LL deep-sleep before Stop1.
6. **CR2032 hardening (D, board-rev hardware).** Add a **47 µF (100 µF cold-climate) X5R/X7R reservoir cap on
   the cell/VIN side** (besides the 10 µF CIN) so the aged/cold cell sees near-DC — cap sources the TX pulse
   (~30 µC burst → ~40 µF holds droop; recharges ~6 ms ≪ report interval). Set **TPS63900 input current limit
   low (10–25 mA)**; consider **VOUT 1.8 V** (validate radio min-VDD). Firmware pulse-spreading = last resort
   only if bench droop demands it.

---

## 5. Hub interface — FROZEN-CONTRACT PREVIEW *(spec handoff; scoped strictly to the leak adv/scan interface)*

The hub work is a **specification deliverable**, not implemented here. Within `app_ble_leak.c` only:
- **Scan:** keep continuous passive ext-scan; keep `filter_duplicates=0`. (If the energy-optimal sensor is
  1 M-only, the Coded scan param becomes optional — but keep dual-PHY for mixed/legacy fleets.)
- **Parse (additive):** read `proto_ver`/`event_type`/`uint16 seq`/`flags` when mfg-data `len ≥ N`; preserve
  existing `company 0x0030`, leak byte, battery uint8 offsets.
- **Dedup & gap-detect:** track `last_seq` per sensor; **dedup** burst repeats (same seq); **`gap = seq −
  last_seq > 1` → emit a missed-report count**; surface staleness/liveness. Ensure the **COMPLETE-only ext-
  report guard (line 237)** never drops the (kept-small) frames; handle **seq reset on sensor reboot**
  (`power_up` event) without a false gap.

Full byte layout + exact rules will be the Gate 4 frozen contract.

---

## 6. Scope-discipline / over-engineering flags

- ❌ **Periodic-adv / PAwR** — infeasible (compiled-out / WBA5-unsupported); don't chase.
- ❌ **Acked retransmit / any sensor RX in steady state** — breaks the broadcaster; hub already listens.
- ❌ **Standby migration** — no EXTI wake, reset-loop risk, illusory saving.
- ❌ **Encrypt the whole payload** — leak/battery aren't secret; if auth is wanted, **CMAC integrity on leak
  edges only**, no CCM/nonce machinery.
- ❌ **Pulse-shaping state machines / supercaps / per-channel ramps** — reservoir cap + average current carry
  the benefit (SWRA349: shaping buys <9 %).
- ⚠️ **The premise itself must be validated:** the size of the event-driven win depends on how much the
  continuous-advertising radio term currently exceeds the Stop1 floor. **If a bench measurement shows the
  system is already Stop1-floor-dominated, the event-driven change yields less and effort should refocus.**

---

## 7. The gating bench measurement + open items

**Highest-value first measurement — idle average current of the *eleak* PCB (Release build):** continuous-adv
baseline vs a Stop1-only floor. This single number (a) quantifies the event-driven win, (b) confirms whether
we're radio-term- or leakage-dominated, and (c) anchors the whole power model. *Do not finalize Gate 4
targets without it.*

Other `bench` items: per-event TX energy at 0 vs 5.6 dBm on 1 M; hub single-advert **miss probability q**
(sets N); VIN droop under a 3-channel burst (cold/aged cell) to size the reservoir cap; AES-CMAC tag
on-air COMPLETE check (if Tier 1); leak→hub end-to-end latency unchanged. `schematic`: 390 K electrode +
standing wet current; battery divider ratio; reservoir-cap placement (cell side).

---

## 8. Safety trade-off — surfaced explicitly (per the brief)

Event-driven advertising **removes the continuous stream**; the safety guarantee then rests on **(a)** the
immediate, decoupled **leak-onset N-burst** (latency held at ~0.6 s) and **(b)** **seq-gap detection +
heartbeat liveness**. This is an architectural change on the safety path — it is **proposed, not assumed**,
and must be **approved (Gate 3) and bench-validated** (leak latency unchanged; target reception probability
met) before any code. No duty-cycle change will gate leak responsiveness without your sign-off.

---
**Gate 2 complete — research only, no firmware changed. Awaiting your review.**
Open product decisions for **Gate 3**: target battery-life figure · heartbeat cadence & worst-case leak
latency · **target reception probability** (sets burst N) · 0 dBm vs 5.6 dBm · **authenticate leak frames?
(Tier 1)** · keep Coded escape hatch? · any future RX/config path? · is the reservoir-cap board-rev in scope?
