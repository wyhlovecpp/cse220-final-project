# CSE220 Final Project — Project Plan

**Team paper:** Paper 3 — J. Kim, S. H. Pugsley, P. V. Gratz, A. L. N. Reddy, C. Wilkerson, Z. Chishti, **"Path Confidence Based Lookahead Prefetching,"** MICRO 2016 (a.k.a. SPP / Signature Path Prefetcher).

**Simulator:** Scarab (`github.com/hpsresearchgroup/scarab`), built locally with PIN 3.15 + a few GCC-13 portability fixes.

---

## 1. Technique Summary

SPP is a confidence-based **lookahead** L2 data prefetcher. Within an OS page, it compresses the sequence of recent intra-page strides ("deltas") into a small 12-bit **signature**. A second table maps each signature to a histogram of subsequent deltas, each with a saturating confidence counter. To predict, SPP performs an iterative *lookahead*: it picks the highest-confidence delta, generates a prefetch, then chains by hashing that delta back into the signature to predict the *next* delta — multiplying confidences along the way. Prefetching stops when the accumulated **path confidence** falls below a threshold, so SPP issues many prefetches when sure and few when uncertain, instead of using a fixed degree like next-line/stride.

Three secondary structures finish the design: a **prefetch filter** (a Bloom-style direct-mapped table) avoids duplicate prefetches and records "useful" feedback; a **global history register (GHR)** lets a lookahead path survive an OS-page boundary by carrying the latest signature/offset/delta forward and re-priming a fresh ST entry on the next page; and a **global accuracy α** (useful/issued) damps confidence when the prefetcher's recent track record is bad.

## 2. Integration into Scarab

Scarab already has a `HWP` framework (`src/prefetcher/pref_common.[ch]`) that exposes per-event callbacks (`ul1_miss`, `ul1_hit`, `ul1_pref_hit`) and a sink queue (`pref_addto_ul1req_queue`) which feeds the L2/LLC MSHRs. We add a new prefetcher slot, `spp`, register it in `pref_table.def`, and wire it up like the existing `stride`/`stream`/`ghb` prefetchers.

- **Input:**
  - `pref_spp_ul1_miss(proc_id, line_addr, loadPC, global_hist)` — L2 demand miss (used as training + lookup trigger).
  - `pref_spp_ul1_hit(proc_id, line_addr, ...)` — L2 hit (used for training only, when `PREF_REPORT_PREF_MATCH_AS_HIT` is on so demands that match outstanding prefetches still train).
  - `pref_spp_ul1_pref_hit(proc_id, line_addr, ...)` — a prefetched line was actually used; we feed this back into the filter to increment global `pf_useful`.
- **Output:** prefetch line addresses pushed via `pref_addto_ul1req_queue(proc_id, line_index, prefetcher_id)` — Scarab schedules them onto the LLC fill path.

## 3. Files to Modify / Add

| Path | Action |
| --- | --- |
| `src/prefetcher/pref_spp.h` | NEW — data structures, externs, `pref_spp_*` API. |
| `src/prefetcher/pref_spp.c` | NEW — full SPP algorithm (ST + PT + Filter + GHR + lookahead). |
| `src/prefetcher/pref_spp.param.def` | NEW — knobs (`PREF_SPP_ON`, table sizes, thresholds, debug). |
| `src/prefetcher/pref_table.def` | EDIT — register `spp` row with the new callbacks. |
| `src/prefetcher/pref_common.c` | EDIT — `#include "prefetcher/pref_spp.h"` (matches existing pattern). |
| `src/CMakeLists.txt` | None — `prefetcher/*.c` is globbed. |
| `PARAMS.spp` (in run dir) | NEW — turn on `--pref_framework_on 1 --pref_spp_on 1`, disable other prefetchers for the SPP runs. |
| `scripts/run_experiments.py` | NEW — orchestrates baseline vs. stride vs. SPP runs, collects stats. |
| `scripts/plot_results.py` | NEW — IPC speedup, MPKI, accuracy/coverage bars. |

## 4. Core Data Structures (paper defaults, configurable)

- **Signature Table:** 1 set × 256 ways, 16-bit tag (per-page), 12-bit signature (`SIG_SHIFT=3`, `SIG_BIT=12`, `SIG_DELTA_BIT=7`), 6-bit `last_offset`, LRU.
- **Pattern Table:** 512 sets × 4 ways, 7-bit `delta`, 4-bit `c_delta`, 4-bit `c_sig` per set.
- **Prefetch Filter:** quotient filter, 1024 entries × (6-bit remainder tag + valid + useful bits).
- **Global History Register:** 8 entries × (valid, 12-bit sig, 8-bit confidence, 6-bit offset, 7-bit delta), plus 10-bit global `pf_useful`/`pf_issued` counters.
- **Thresholds:** `PF_THRESHOLD = 25` (% — issue), `FILL_THRESHOLD = 90` (% — fill L2 vs LLC).

Total cost ≈ 6 KB — within the paper's 6 KB budget claim.

## 5. Algorithm Sketch (per L2 demand)

```
on ul1_miss(addr, PC):
    page, offset = split(addr)
    last_sig, curr_sig, delta = ST.read_and_update_sig(page, offset)   # 12-bit rolling hash
    FILTER.check(addr, L2C_DEMAND)                                     # bumps useful counter
    if last_sig: PT.update_pattern(last_sig, delta)                    # confidence training
    base_addr   = addr
    look_conf   = 100
    do:
        for each (delta, c_delta) in PT[curr_sig]:
            path_conf = (depth==0) ? local : α * c_delta/c_sig * look_conf
            if path_conf >= PF_THRESHOLD: enqueue path_conf, delta
        pick highest-confidence way → look_conf, drive next iteration
        for each pending (pf_addr, conf):
            if same page: FILTER.check → pref_addto_ul1req_queue
            else (page-cross): GHR.update_entry(curr_sig, conf, off, delta); stop
        curr_sig = (curr_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK
        base_addr += chosen_delta * BLOCK_SIZE
    while at least one prefetch was issued this iteration
```

## 6. Baselines & Evaluation Methodology

- **B1 — No prefetcher** (`--pref_framework_on 1` with all prefetchers off).
- **B2 — Stride** (`--pref_stride_on 1`) — Scarab's RPT-style stride prefetcher; closest "simple" baseline per the project spec.
- **B3 — Stream** (`--pref_stream_on 1`) — Scarab's default stream prefetcher (sanity check, since the canonical `PARAMS.kaby_lake` uses it).
- **B4 — SPP (ours)** (`--pref_spp_on 1`).

**Workloads:** start with the hand-written assembly tests in `src/test/` (e.g., `simple_loop`) and the `qsort` mini-benchmark; then run on a handful of memory-intensive micro-traces we generate (linked-list traversal, matrix multiply, hash-table probe) compiled with `-march=nehalem` to avoid BMI2 instructions that Scarab's PIN-side decoder doesn't recognise. Each run uses a fixed instruction limit (e.g., 100 M for warmup-bypassed ROI).

**Metrics:** IPC (and speedup over B1), L2 MPKI, prefetcher **accuracy** = `pref_useful / pref_sent`, **coverage** = `pref_useful / (pref_useful + ul1_misses)`, and **timeliness** = `late_prefetch / pref_useful`. Scarab already reports these via the prefetcher stat hooks (`STAT_EVENT` in `pref_common.c`).

## 7. Risks & Plan B

- If page-cross handling (GHR) is buggy → disable it via `--pref_spp_ghr_on 0`, paper still works (lower coverage).
- If lookahead is wrong → cap `--pref_spp_max_depth 1` so it degrades to a 1-step confidence-filtered Markov prefetcher.
- If we can't get long enough traces in time → evaluate on a synthetic microbenchmark suite (stride, indirect, irregular gather), and clearly label the result.
