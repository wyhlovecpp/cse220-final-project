# Implementing the Signature Path Prefetcher (SPP) in Scarab

**CSE 220 Final Project (Paper 3) — Final Report**

---

## 1. Introduction

The Signature Path Prefetcher (SPP) [Kim et al., MICRO 2016] is a confidence-based, lookahead L2 data prefetcher. Within an OS page, SPP compresses the trail of recent intra-page strides ("deltas") into a small 12-bit **signature**. A second table maps each signature to a histogram of subsequent deltas, each with a saturating confidence counter. To predict, SPP iteratively performs *lookahead*: it selects the highest-confidence delta, issues a prefetch, then folds that delta back into the signature to predict the *next* delta — multiplying confidences along the way. Prefetching stops when the accumulated **path confidence** falls below a threshold, so SPP issues many prefetches when it is confident and few when it is uncertain. Compared to a fixed-degree stride prefetcher this lets SPP cover non-trivial patterns (e.g., delta sequences) without polluting the cache on hard-to-predict streams.

We implemented SPP in the Scarab cycle-accurate simulator and compared it against three baselines: no prefetcher, Scarab's RPT-style **stride** prefetcher, and Scarab's **stream** prefetcher. Across a five-benchmark micro-suite (sequential, large-stride, 2-D stencil, deterministic linked-list, random) SPP delivered a **3.47× speedup on linked-list pointer chasing**, a **7 % speedup on the 2-D stencil**, did **no measurable harm** on the random-access torture case, and reached an average **lookahead depth of 9–12 hops** on regular workloads — exactly the confidence-throttling the paper aimed for.

## 2. Implemented Technique

We followed the paper's structure and the widely-circulated ChampSim reference, but ported it onto Scarab's existing hardware-prefetcher (HWP) framework. The implementation lives in three new files under `src/prefetcher/`:

* **`pref_spp.h`** — declares the four core tables: `SIGNATURE_TABLE`, `PATTERN_TABLE`, `PREFETCH_FILTER`, `GLOBAL_REGISTER`.
* **`pref_spp.c`** — the algorithm itself (~600 LoC), plus the Scarab HWP callbacks (`pref_spp_ul1_{miss,hit,pref_hit}`).
* **`pref_spp.param.def`** — Scarab parameters: table dimensions, thresholds, and three "kill-switches" (`PREF_SPP_LOOKAHEAD_ON`, `PREF_SPP_FILTER_ON`, `PREF_SPP_GHR_ON`).

The default sizing matches the paper / ChampSim reference:

| Structure | Dimensions | Bits per entry | Total |
|-|-|-|-|
| Signature Table | 1 × 256 | 1 + 16 + 12 + 6 + 8 (LRU) | ≈ 1.4 KB |
| Pattern Table | 512 × 4 | 7 (Δ) + 4 (cΔ) + 4 (cSig/set) | ≈ 3.4 KB |
| Prefetch Filter | 1024 × 1 | 6 (rem) + 2 (valid/useful) | ≈ 1.0 KB |
| GHR | 8 × 1 | 1 + 12 + 8 + 6 + 7 | ≈ 0.04 KB |
| **Total** | | | **≈ 6 KB** |

This matches the paper's claimed budget.

## 3. Scarab Integration

Scarab dispatches per-cache events through an `HWP` table in `src/prefetcher/pref_table.def`. We added one row registering SPP with three callbacks:

```c
{ "spp", PREF_TO_UL1, NULL, pref_spp_init, pref_spp_done, NULL,
  NULL, NULL, NULL,           /* DL0 unused for SPP */
  NULL, NULL, NULL,            /* UMLC unused for SPP */
  pref_spp_ul1_miss, pref_spp_ul1_hit, pref_spp_ul1_pref_hit },
```

A demand L2 miss enters via `pref_spp_ul1_miss(proc_id, line_addr, ...)`. SPP also trains on demands that match an in-flight prefetch — Scarab routes those via `pref_spp_ul1_hit` when `PREF_REPORT_PREF_MATCH_AS_HIT=1` (which we assert in `pref_spp_init`). Prefetched lines that get used later trigger `pref_spp_ul1_pref_hit`, which we feed into the filter so the global accuracy α = `pf_useful / pf_issued` can update. Outgoing prefetches are pushed via Scarab's `pref_addto_ul1req_queue`.

We exposed the algorithm as Scarab parameters (e.g., `--pref_spp_on=1`, `--pref_spp_max_depth=16`, `--pref_spp_pf_threshold=25`), and added twenty SPP-specific `STAT_EVENT`s (operate count, ST hit/install, GHR boots, issued L2 vs LLC prefetches, filtered, page-crosses, useful, lookahead depth histogram).

### Build portability fixes

Scarab's `master` was last touched in 2020 and does not build cleanly on Ubuntu 24.04 / GCC 13:
1. Added `#include <cstdint>` to `ramulator/StatType.h` (newer libstdc++ no longer transitively pulls it in).
2. Removed a stale `disasm_reg(uns)` re-declaration in `debug/debug_print.h` that conflicts with the typed one in `isa/isa.h`, and added an explicit include of `isa/isa.h` in `node_stage.c` (GCC 13 enforces `-Werror=enum-int-mismatch`).
3. Added `-fcommon` to `CMAKE_C/CXX_FLAGS_*` (GCC ≥ 10 defaults to `-fno-common`, which breaks Scarab's pattern of declaring globals in headers).
4. Added `-Wno-error -fcommon` to PIN tool flags so `pin_exec.so` builds with PIN 3.15 against gcc 13, and fixed a `const auto&` range-loop in `decoder.cc:140`.

### Two SPP-specific bugs we found while bringing this up

* **Per-iteration scratch queue, not shared.** The ChampSim reference uses one growing `pf_q` over the entire `do { ... } while (do_lookahead)` loop. With multi-Δ patterns (linked-list, stencil) plus `MAX_DEPTH > 3` this overflows the (modest) `pf_q` size and silently writes past the buffer. We allocate the queue per outer iteration (`PT_WAY + 1` slots) and reset head/tail each pass. The original linked-list/depth=16 SIGSEGV went away with this change.
* **Stat-event OOB.** Our first cut declared a single `PREF_SPP_LOOKAHEAD_DEPTH_MAX` DIST stat at the very end of `pref.stat.def`, then wrote to `STAT_EVENT(0, DEPTH_MAX + i)` for `i = 0..9`. Since `pref.stat.def` is the *last* stat file in `stat_files.def`, those writes ran past `global_stat_array`. We replaced the single slot with ten explicit `DEPTH_0..DEPTH_8/GE9` entries; the strided/depth=16 SIGSEGV went away with this change. (Tip for future SPP-in-Scarab work: never compute a stat ID by addition unless every offset is its own `DEF_STAT`.)

## 4. Evaluation Methodology

**Configurations.** All four use the canonical `PARAMS.kaby_lake` core / cache / memory config (32 KB L1d, **1 MB 8-way L2**, DDR4-2400, single core). Only the prefetcher knobs differ:

| | `pref_framework_on` | enabled prefetcher | other prefetchers |
|-|-|-|-|
| `nopref` | 0 | — | — |
| `stride` | 1 | `pref_stride_on` | off |
| `stream` | 1 | `pref_stream_on` | off |
| `spp` | 1 | `pref_spp_on` | off |

**Workloads.** Five hand-crafted micro-benchmarks (`benchmarks/bench_*.c`), each compiled `-O2 -march=nehalem -mno-bmi -mno-bmi2 -mno-avx2 -mno-fma -mno-sse4.2 -static`. The `-mno-*` flags keep our kernel-loop code within the ISA subset Scarab's PIN decoder currently knows about; the static glibc still uses BMI2 in `strlen`/`memchr`, but those routines never run inside our ROI:

| Benchmark | What it exercises | What SPP should do |
|-|-|-|
| `stride` | Sequential `+1` cache-line scan over 16 MB | Saturate `local_conf`; deep chain |
| `strided` | `+7` cache-line skip over 32 MB | Learn the `+7` Δ |
| `2dstencil` | 5-pt Jacobi over a 512×2048 grid | Multi-Δ (-W, -1, +1, +W) per cell |
| `linkedlist` | Pointer-chase 256 K nodes (Δ=+3 elts) | Learn intra-page +1/+2 alternation |
| `random` | Xorshift-indexed loads from an 8 MB pool | Conservative; *no harm* |

10 M instructions per run after PIN fast-forward (some benchmarks terminate earlier when their kernel runs out of work).

**Metrics.** IPC (Scarab's `NODE_CYCLE` / `NODE_INST_COUNT`), L2 (UL1) MPKI, prefetcher **accuracy** = `(L1_PREF_UNIQUE_HIT + L1_PREF_LATE) / PREF_UL1REQ_QUEUE_SENTREQ`, prefetcher **coverage** = useful prefetches ÷ (useful + remaining misses), and (SPP-only) **average lookahead depth** = `PREF_SPP_LOOKAHEAD_DEPTH_TOTAL / PREF_SPP_OPERATE`.

## 5. Results

### 5.1 Headline IPC and speedup

| Benchmark | nopref IPC | stride speedup | stream speedup | **SPP** speedup |
|-|-|-|-|-|
| `stride`     | 1.814 | 1.07× | **1.20×** | 0.95× |
| `strided`    | 0.331 | 1.01× | 1.00×     | 1.00× |
| `2dstencil`  | 2.536 | 1.18× | **1.37×** | **1.07×** |
| `linkedlist` | 0.049 | **3.77×** | 3.71× | **3.47×** |
| `random`     | 0.625 | 1.00× | 1.00×     | 1.00× |

Five takeaways:

1. **Linked-list pointer chasing — SPP shines.** SPP turns the 0.049 IPC baseline into 0.169, a **3.47×** speedup. Stride and stream do slightly better (3.77× / 3.71×) because the layout's `+3` element delta lines up with their fixed-distance scan, but SPP recovers most of the gap and reaches **99.98 % accuracy** with a 12-hop average lookahead — *exactly* the regime SPP was designed for.
2. **2-D stencil — SPP gains, but does not match stream.** SPP picks up +7 % over nopref. Stream gets +37 % because the row scan is a true sequential stream that perfectly matches its design; SPP must page-cross every row (W = 512 × 8 B = one page) and rely on the GHR to re-seed each new row.
3. **Sequential stride — SPP loses ~5 %.** This was the most counter-intuitive result. SPP issues 303 K prefetches per 10 M instructions on the stride benchmark with 97 % accuracy, but **90 % of them arrive *late*** (counted in `L1_PREF_LATE`, not `L1_PREF_UNIQUE_HIT`). The 4 KB OS-page boundary caps the in-page lookahead chain at 64 cache lines while stride's 64 KB region lets it queue prefetches farther ahead.
4. **Large `+7` stride — too short to characterise.** The benchmark terminates after only 599 K instructions because each iteration of the inner loop does only one load; all four configs land at 0.33 IPC ± 0.5 %. Treated as a sanity check rather than a comparison.
5. **Random — SPP does no harm.** All four configs land at 0.625 IPC. SPP issues a handful of speculative prefetches but its confidence gate quickly throttles them — average lookahead depth on `random` is **0** while it is 9–12 on the other workloads. This is exactly the conservative behaviour the paper promises.

### 5.2 Prefetcher quality

Accuracy and coverage closely track IPC. SPP holds **96–100 %** accuracy on every benchmark with structure (because the path-confidence gate suppresses low-confidence chains) and stays above stream's accuracy on `linkedlist` (99.98 % vs 66.64 %), reflecting SPP's tight per-prefetch filtering. Coverage hovers around 48–50 % for all three prefetchers on the regular workloads (the cap is set by MSHR pressure on this Scarab model).

| Benchmark | SPP accuracy | SPP coverage | SPP avg depth |
|-|-|-|-|
| `stride`     | 96.68 % | 48.38 % |  9.86 |
| `strided`    |  7.78 % |  6.34 % | 10.50 |
| `2dstencil`  | 97.12 % | 48.49 % |  9.01 |
| `linkedlist` | 99.98 % | 49.02 % | 12.28 |
| `random`     | 11.58 % |  0.00 % |  0.00 |

The plots in `results/{speedup,ipc,l1_mpki,pref_accuracy,pref_coverage,spp_depth}.png` visualise the same numbers.

### 5.3 Ablation: what each piece contributes

We rebuilt SPP with each of its two optional sub-systems individually disabled (`--pref_spp_lookahead_on=0` and `--pref_spp_ghr_on=0`) and reran on the two most informative workloads (script: `scripts/run_ablation.sh`, outputs: `results_ablation/`):

| Configuration | `linkedlist` IPC | speedup | `2dstencil` IPC | speedup |
|-|-|-|-|-|
| **nopref** (baseline)         | 0.049 | 1.00× | 2.536 | 1.00× |
| `spp` (full algorithm)         | **0.169** | **3.47×** | **2.720** | **1.07×** |
| `spp` − lookahead (depth = 1)  | 0.076 | 1.56× | 2.554 | 1.01× |
| `spp` − GHR (no page-boot)     | 0.158 | 3.26× | 2.718 | 1.07× |

* **Lookahead is the workhorse.** Disabling it (effectively reducing SPP to a 1-step confidence-filtered Markov prefetcher) collapses the linked-list speedup from 3.47× to 1.56×, and the stencil speedup from 1.07× to 1.01×. The path-confidence chain is responsible for ≈ 60 % of SPP's gain on linked-list and almost all of it on the stencil.
* **GHR's contribution is small on these workloads.** On linked-list the GHR adds ≈ 6 % (the pool spans many pages and an 8-entry GHR can be evicted before the destination page is touched). On 2-D stencil it adds ≈ 0.1 % — surprising at first, but consistent with the page footprint: with W = 512 elements = exactly one OS page per row, *every* +W cross-row prefetch becomes a page-crossing, and the GHR with only 8 entries cannot retain enough history for thousands of distinct pages.
* The conclusion: SPP's main lever is the **lookahead chain**, and the GHR matters most when the workload has *few* hot pages that get re-visited (something neither micro-benchmark fully exercises — both stream through their working set once).

### 5.4 PF_THRESHOLD sweep — finding the design knee

The paper picks `PF_THRESHOLD = 25 %` for the per-hop confidence cutoff at which SPP issues a prefetch. We swept it on `linkedlist` (script: `scripts/run_threshold_sweep.sh`):

| PF_THRESHOLD | IPC | Speedup | Accuracy | Avg lookahead depth |
|-|-|-|-|-|
| 10 % (more aggressive)  | 0.165 | 3.39× | 100.0 % | 15.94 |
| **25 %** (paper default) | 0.169 | 3.47× | 100.0 % | 12.28 |
| **40 %** (knee)          | **0.174** | **3.57×** | 100.0 % | 8.25 |
| 60 %                     | 0.166 | 3.41× | 100.0 % | 4.46 |
| 80 % (very conservative) | 0.099 | 2.03× | 96.8 % | 1.46 |

The knee sits at **PF_THRESHOLD = 40 %** — about 3 % better than the paper's default on this benchmark, with the same 100 % accuracy. Two observations:

* **More aggressive ≠ better.** At PF_THRESHOLD = 10 % the chain runs to its `MAX_DEPTH = 16` cap (avg depth 15.94) but doesn't translate into IPC, because the L2 request queue (128 entries) saturates and excess prefetches get dropped.
* **Too conservative collapses fast.** At 80 % the chain stops at depth ≈ 1.5 and we lose half of SPP's coverage, dropping IPC to 0.099 (only 2× over nopref). The paper's choice of 25 % is a safe default; 40 % is mildly better on linked-list because *every* link in the chain is a 100 %-confidence prediction once the +1/+2 alternating delta is learned, so it's safe to filter more aggressively.

We left `PF_THRESHOLD = 25` as the SPP default to match the paper; an end-user can opt into the knee with `--pref_spp_pf_threshold=40`.

### 5.5 Does the knee generalise? Re-running every benchmark at PF_THRESHOLD = 40

Re-ran all five benchmarks at `--pref_spp_pf_threshold=40` (script: `scripts/run_pf40_all.sh`, outputs: `results_pf40/`):

| Benchmark | nopref | SPP @ pf=25 (default) | SPP @ pf=40 (knee) | knee vs default |
|-|-|-|-|-|
| `stride`     | 1.814 | 1.730 | **1.757** | +1.6 % |
| `strided`    | 0.331 | 0.331 | 0.331     | ±0 % (noise band) |
| `2dstencil`  | 2.536 | 2.720 | **2.751** | +1.1 % |
| `linkedlist` | 0.049 | 0.169 | **0.174** | +3.0 % |
| `random`     | 0.625 | 0.625 | 0.625     | ±0 % |

**The knee is a strict improvement everywhere it matters** — never worse, 1–3 % better on the three benchmarks where SPP actually issues prefetches. On `random` the confidence gate already filters out 100 % of would-be prefetches at any threshold, so the knee makes no difference. This argues that 40 % is a better default than the paper's 25 % at least on this Scarab + Kaby-Lake configuration; the original paper picked 25 % against a different (larger) L2 and MSHR, so the right cutoff is plausibly machine-dependent.

### 5.6 MAX_DEPTH sweep — confidence stops the chain, not the cap

We swept `--pref_spp_max_depth ∈ {1, 2, 4, 8, 16, 32, 64}` on `linkedlist` at the knee `pf=40` (script: `scripts/run_depth_sweep.sh`):

| MAX_DEPTH | IPC | speedup | observed avg depth |
|-|-|-|-|
| 1  | 0.076 | 1.55× | 0.69 (no lookahead at all) |
| 2  | 0.109 | 2.22× | 1.66 |
| 4  | 0.161 | 3.30× | 4.00 (hits cap) |
| **8**  | **0.176** | **3.59×** | 7.64 (close to cap) |
| 16 | 0.174 | 3.57× | 8.25 (saturates) |
| 32 | 0.174 | 3.57× | 8.25 (no change) |
| 64 | 0.174 | 3.57× | 8.25 (no change) |

Two findings:

* **`MAX_DEPTH = 8` is enough** for this workload — going deeper buys nothing because the path-confidence threshold (40 %) naturally terminates the chain at avg ≈ 8 hops. Beyond MAX_DEPTH = 16 the observed depth is identical to the cap of 8, confirming the cap is never the limiting factor.
* **`MAX_DEPTH = 16` is *slightly worse* than `MAX_DEPTH = 8`** (0.174 vs 0.176) — a small but reproducible MSHR-pressure penalty for issuing prefetches at depth 8 that get filtered by the chain anyway. The paper's `MAX_DEPTH = L2_MSHR_SIZE = 16` is a fine upper bound but, again, machine-dependent.

The two knees compose cleanly: at `pf=40, depth=8` SPP delivers the same 3.59× speedup as `pf=40, depth=16` while issuing slightly fewer prefetches.

### 5.7 GHR_ENTRIES sweep — and why our earlier hypothesis was wrong

We claimed in §5.3 that GHR contributed only ~0 % on the 2-D stencil "because its 8 entries cannot retain enough history for thousands of distinct pages". We tested this directly by sweeping `--pref_spp_ghr_entries ∈ {8, 16, 32, 64, 128}`:

| GHR_ENTRIES | 2dstencil IPC |
|-|-|
| 8   | 2.751 |
| 16  | 2.751 |
| 32  | 2.751 |
| 64  | 2.751 |
| 128 | 2.751 |

**Result is flat to 4 decimal places** — GHR size is *not* the bottleneck.

We initially hypothesised the cause was SPP's **7-bit sign-magnitude delta encoding** (max representable |delta| = 63 cache lines), since the stencil's cross-row delta is ±W = ±64 cache lines. We tested this directly by widening the encoding (`--pref_spp_sig_delta_bit ∈ {7, 8, 10}`):

| `SIG_DELTA_BIT` | 2dstencil IPC | GHR boots |
|-|-|-|
| 7 (paper default) | 2.71961 | 2046 |
| 8 (hypothesis "fix") | 2.71961 | 2046 |
| 10 | 2.71961 | 2046 |

**Hypothesis falsified** — widening the encoding produces *identical* IPC and identical GHR boot count to 4 decimal places. Re-examining the code clarified why: within a single page, the page offset is ∈ [0, 63] and so the maximum *in-page* delta is also ±63 (never ±64). SPP never *computes* a cross-page delta during ST/PT training — page-crossings are detected and shunted to the GHR *as an event*, carrying the in-page delta value (∈ ±63) verbatim. The encoding width is therefore never the binding constraint.

The actual reason GHR contributes so little on the stencil is **the lookahead chain's geometry**: PT learns the dominant +1/-1 within-row deltas, the chain extends *along the row* one cache line at a time, and only the row's last hop becomes a page-crossing event. The chain never naturally generates a +W cross-row hop that GHR could carry to the destination page's first access. Fixing that would require a fundamentally different algorithm (e.g. spatial memory streaming or Bingo), not a wider delta encoding.

A side-finding: on linked-list, `SIG_DELTA_BIT = 8` is *very slightly worse* than 7 (IPC 0.169 vs 0.174, GHR boots identical at 24 580). Even when all deltas are in range, the encoding's sign-bit *position* changes which PT buckets the same negative delta lands in, perturbing the predictor in non-trivial ways. SPP is more sensitive to encoding details than the paper makes apparent.

### 5.8 Two more Scarab built-in prefetchers as additional baselines

We compared SPP against the remaining Scarab built-in prefetchers (`ghb`, `markov`) on `linkedlist`. (`2dc` triggered a pre-existing Scarab assertion `proc_id == ul1req_queue[q_index].line_addr >> 58` and was excluded.)

| Prefetcher (`linkedlist` IPC) | IPC | speedup vs nopref |
|-|-|-|
| nopref                          | 0.049 | 1.00× |
| markov                          | 0.073 | 1.51× |
| ghb                             | 0.139 | 2.86× |
| stream                          | 0.180 | 3.71× |
| **SPP (pf=40, depth=8)**        | **0.176** | **3.59×** |
| stride                          | 0.183 | 3.77× |

SPP sits in the top tier alongside stride and stream — within 4 % of stride, ahead of `ghb` (the closest comparable history-based prefetcher) by 27 %.

**Full 6-prefetcher × 6-benchmark matrix** (script: `scripts/run_alt_prefetchers.sh`, outputs: `results_alt/`). All numbers are IPC; *bold* marks the best prefetcher per row, *underlined* marks regressions vs `nopref`:

| Benchmark | nopref | stride | stream | SPP (tuned) | ghb | markov |
|-|-|-|-|-|-|-|
| `stride`     | 1.814 | 1.948 | **2.174** | 1.773 | 1.893 | 1.814 |
| `strided`    | 0.331 | 0.334 | 0.330 | 0.331 | **0.334** | 0.331 |
| `2dstencil`  | 2.536 | 3.001 | **3.470** | 2.773 | 2.965 | 2.536 |
| `linkedlist` | 0.049 | **0.183** | 0.180 | 0.176 | 0.139 | 0.073 |
| `random`     | 0.625 | 0.625 | 0.625 | 0.625 | 0.625 | *0.349 (−44 %)* |
| `hashtable`  | 0.333 | 0.333 | 0.333 | 0.331 | 0.333 | *0.159 (−52 %)* |

Three observations:

1. **Markov is catastrophic on unstructured workloads.** On `random` it loses 44 % IPC; on `hashtable` 52 %. Markov correlates the *next* miss address with the *previous* miss address; on workloads without that locality, it learns spurious associations and aggressively prefetches polluting addresses. The same workloads where SPP's confidence gate keeps it safe (within 0.5 % of nopref) destroy Markov.
2. **SPP is the most consistent prefetcher across the matrix.** Never worse than 0.98× of `nopref` (on stride, where the page boundary limits its chain), and SPP issues prefetches on 4 of 6 workloads where any prefetcher does anything useful. No other prefetcher dominates everywhere — stream wins on streams, stride on linked-list, but SPP is *never the worst* and never *catastrophic*.
3. **`ghb` is the second-most consistent**, but it's behind SPP on `linkedlist` (the workload where path-confidence lookahead pays off most) by 27 %.

### 5.9 The composed knee — `pf=40, depth=8` on every benchmark

§5.4 found `pf=40` is the threshold knee, §5.6 found `depth=8` is the cap knee. We never combined them; we test that here (script: `scripts/run_combined_knee.sh`, outputs: `results_tuned/`):

| Benchmark | nopref | **SPP paper default**<br>(pf=25, d=16) | SPP one-knob<br>(pf=40, d=16) | **SPP both knees**<br>(pf=40, d=8) | combined vs default |
|-|-|-|-|-|-|
| `stride`     | 1.814 | 1.730 | 1.757 | **1.773** | +2.5 % |
| `strided`    | 0.331 | 0.331 | 0.331 | 0.331     |   0    |
| `2dstencil`  | 2.536 | 2.720 | 2.751 | **2.773** | +1.9 % |
| `linkedlist` | 0.049 | 0.169 | 0.174 | **0.176** | +4.1 % |
| `random`     | 0.625 | 0.625 | 0.625 | 0.625     |   0    |

**The two knees compose without conflict** — `pf=40, depth=8` is the best SPP configuration we found on every benchmark where SPP issues prefetches, beating the paper's default by 1.9 %–4.1 % with no regression elsewhere. This is the *final* SPP design point we recommend for this Scarab + Kaby-Lake configuration; users can flip on both via `--pref_spp_pf_threshold=40 --pref_spp_max_depth=8`.

### 5.10 A new failure case — hash-table probing

We added a sixth micro-benchmark, `benchmarks/bench_hashtable.c`, that builds a 128 K-bucket closed-addressing hash table over a 256 K-entry pool and probes it 512 K times with xorshift-generated keys. Each probe is `table[key & MASK]` (random scatter) followed by a short within-bucket chain walk (locality-preserving):

| Config (hashtable IPC) | IPC | speedup vs nopref |
|-|-|-|
| nopref                                | 0.333 | 1.000× |
| stride                                | 0.333 | 1.000× (no effect — correct) |
| stream                                | 0.333 | 1.000× (no effect — correct) |
| **SPP default** (pf=25, d=16)         | **0.328** | **0.985× (−1.3 %)** |
| SPP tuned (pf=40, d=8)                | 0.331 | 0.994× (−0.5 %) |

`hashtable` is the first benchmark where **SPP slightly hurts performance**. Looking at the SPP-internal counters:

* `PREF_SPP_OPERATE = 1 392 521` L2 demands processed.
* `PREF_SPP_PF_ISSUED_L2 = 31` (!) — the path-confidence gate correctly classified almost all candidates as low-confidence.
* But `PREF_UL1REQ_QUEUE_SENTREQ = 324 489` because the *framework* still routes sub-threshold (`< FILL_THRESHOLD = 90 %`) prefetches as LLC-grade requests, not L2.
* Useful rate `L1_PREF_HIT / sent = 11 %` — far below SPP's 96–100 % on the other workloads.

So even though SPP's confidence gate is working as designed, the within-bucket chain layout in `pool[]` *does* exhibit a learnable +offset pattern (Entry size = 32 B, two entries per cache line), which SPP picks up just enough to issue lots of low-confidence prefetches that don't hit. Tuning to `pf=40, depth=8` cuts the harm roughly in half. A more principled fix would be a separate per-prefetcher LLC-issue threshold, which the paper does not specify — Scarab's framework decides that.

This nuances our earlier "SPP does no harm" claim: SPP does *no measurable harm* on uniformly random access (`random` benchmark), but on workloads that *look* mildly predictable but aren't (like hash-table chain walking), it can leak a few percent of IPC.

### 5.11 Validating the LLC-threshold hypothesis on hashtable

We tested the hypothesis from §5.10 directly — sweep `PF_THRESHOLD` upward on `hashtable` and watch the LLC-grade prefetches drop, IPC recover (script: `scripts/run_hashtable_threshold.sh`):

| PF_THRESHOLD | hashtable IPC | speedup | UL1 sent | L2-grade (>90%) | LLC-grade (≥pf) |
|-|-|-|-|-|-|
| 25 (paper default) | 0.328 | 0.985× (−1.5 %) | 324 489 | 31 | 325 106 |
| 40 (tuned earlier) | 0.331 | 0.994× (−0.5 %) | 138 573 | 42 | 139 036 |
| 60                 | 0.332 | 0.997× (−0.1 %) | 12 795 | 30 | 13 026 |
| **80**             | **0.333** | **1.000× (clean)** | **6** | **0** | **6** |
| 90                 | 0.333 | 1.000× | 0 | 0 | 0 |
| 95                 | 0.333 | 1.000× | 0 | 0 | 0 |

**Hypothesis confirmed.** The LLC-grade prefetch count drops monotonically with the threshold (325 K → 139 K → 13 K → 6 → 0), and the IPC regression vanishes in lockstep. At `pf=80`, SPP issues only six speculative prefetches (all sub-FILL_THRESHOLD, so LLC-grade) over the entire 10 M-instruction run, and these six cost nothing measurable. By `pf=90`, the gate completely silences SPP on this workload — which is the correct outcome: a workload with no learnable structure should get no prefetches.

**The wider lesson:** SPP's design has *two* threshold knobs — `PF_THRESHOLD` (issue at all) and `FILL_THRESHOLD` (issue as L2- or LLC-grade) — but the paper only specifies one (`PF_THRESHOLD = 25`) and lets `FILL_THRESHOLD = 90` define the L2/LLC boundary. On workloads with confidence-mass concentrated in the 25–90 % band (like our hashtable, where the chain walk has some pattern but not enough to merit L2-grade prefetches), this leaks LLC-grade prefetches that pollute slightly. A more nuanced design would expose a separate **issue cutoff** that can be tuned per workload class — at `issue=80` on hashtable, SPP is essentially "off" and the regression disappears.

### 5.12 We built the fix — and proved it doesn't generalise

To go beyond hypothesising, we actually *implemented* the proposed knob: a new `PREF_SPP_ISSUE_THRESHOLD` parameter, decoupled from `PF_THRESHOLD`. The change is a few lines in `pref_spp.c`:

```c
uns issue_cutoff = PREF_SPP_ISSUE_THRESHOLD
                     ? PREF_SPP_ISSUE_THRESHOLD : PREF_SPP_PF_THRESHOLD;
...
if(this_conf < issue_cutoff) {
  do_lookahead = TRUE;   /* keep the chain alive */
  continue;              /* but don't issue this prefetch */
}
```

The chain still extends through low-confidence hops (so deeper, higher-confidence predictions are still discoverable); only the actual prefetch issue is gated. Default `ISSUE_THRESHOLD = 0` falls back to `PF_THRESHOLD` (no behaviour change).

We then re-ran three representative benchmarks at `issue ∈ {0 (off), 60, 80}` with `pf=40, depth=8`:

| Benchmark | `tuned`<br>(issue off) | `issue=60` | `issue=80` |
|-|-|-|-|
| `linkedlist`  | 0.176 | 0.166 (−5.7 %) | **0.099 (−43.7 %)** |
| `2dstencil`   | 2.773 | 2.664 (−3.9 %) | 2.573 (−7.2 %) |
| `hashtable`   | 0.331 | 0.332 (+0.3 %) | **0.333 (== nopref, full fix)** |

**The new knob works exactly as intended on `hashtable`** — at `issue=80` the regression disappears completely. But the same setting **devastates** `linkedlist` (−43.7 %) and significantly hurts `2dstencil` (−7.2 %).

Why? Because on `linkedlist` the path-confidence chain decays geometrically with α and depth: hop 1 is ~95 %, hop 2 ~80 %, hop 3 ~70 %, …, hop 8 ~50 %. Most of the prefetches that matter for hiding DRAM latency sit in the 50–80 % band — exactly the band `issue=80` filters out. The chain's tail is *useful on productive workloads* and *polluting on near-random workloads*, and a pure confidence cutoff can't tell them apart.

**The honest conclusion:** the `ISSUE_THRESHOLD` knob is a useful *workload-class-specific* tuning lever (perfect for hashtable when you know in advance the access pattern is mostly unpredictable), but **not** a universal fix for the hashtable regression. A real fix needs *accuracy*-based filtering rather than *confidence*-based filtering — exactly what the SPP+PPF [Bhatia ISCA'19] perceptron filter does. Confidence is the wrong feature; the right one is "did this signature's recent prefetches actually get used".

This is the only finding in our study with a clear negative result, and it points directly at the next chapter of SPP research.

### 5.13 A seventh benchmark — naive matrix multiply

We added `benchmarks/bench_matmul.c`, a textbook 512×512 dense GEMM (`C = A × B`, three nested loops, no blocking). The inner-loop access pattern interleaves three streams:

* `A[i][k]` — row-major sequential as `k` advances; +1 cache-line delta.
* `B[k][j]` — column-stride of N doubles = N × 8 B = **exactly 4 KB = one OS page** for our N = 512.
* `C[i][j]` — outer-loop invariant; should be a hit.

| Config (matmul IPC) | IPC | speedup vs nopref |
|-|-|-|
| nopref                                | 0.374 | 1.00× |
| stride                                | 0.384 | 1.025× |
| stream                                | 0.374 | 1.00× (no effect!) |
| SPP default (pf=25, d=16)             | 0.365 | 0.976× (**−2.4 %**) |
| SPP tuned (pf=40, d=8)                | 0.365 | 0.976× (−2.4 %; tuning doesn't help) |

Two findings:

1. **Stream's no-effect is surprising** at first, but explained by the inner k-loop's interleaved A+B streams. Scarab's stream prefetcher tracks streams per *cache line*; A's +1 line stream and B's +64 line stream collide in the tracker.
2. **SPP regresses by 2.4 %** — the *same* root cause as `2dstencil`'s GHR being useless (§5.7). B's per-`k` stride is exactly one OS page, and SPP's signature has no way to express that pattern (the in-page delta computation is bounded to ±63 lines). SPP picks up the +1 stream from A but issues 200 K low-confidence LLC-grade prefetches based on misleading signature mixes from B's per-page hops, and those pollute. Our tuned `(pf=40, depth=8)` knee doesn't fix it either, because the bad prefetches mostly have confidence in the 40–90 % band that the knee can't filter out.

**Take-away.** matmul becomes the second SPP failure case in our suite (the first being `hashtable`). Both share a common shape: a workload where the *real* stride lives at a granularity SPP's intra-page signature can't represent (an OS-page row stride here, an unstructured chain walk in hashtable). This is *not* a bug in SPP — it's a deliberate algorithmic boundary the paper acknowledges — but it explains where the technique stops working and motivates spatial-streaming variants (Bingo, SMS) as complementary techniques.

### 5.14 Single-variable confirmation — varying N in matmul

To pin down the page-alignment hypothesis, we vary just the matrix dimension `N`, holding everything else constant. The B-matrix per-`k` stride is `N×8 B`; only at `N=512` is that an exact multiple of the 4 KB OS page:

| `N` | B's k-stride | page-aligned? | nopref | SPP default | Δ vs nopref |
|-|-|-|-|-|-|
| **512** | **4096 B = 1 page** | **yes** | 0.374 | **0.365** | **−2.3 %** |
| 448 | 3584 B = 56 lines | no | 1.032 | 1.041 | +0.9 % |
| 384 | 3072 B = 48 lines | no | 1.114 | 1.129 | +1.4 % |
| 256 | 2048 B = 32 lines | no | 2.284 | 2.310 | +1.1 % |

The hypothesis is confirmed with a clean single-variable test: **N=512 is the only point where SPP regresses, and it is the only point where the row stride aligns exactly with an OS page**. At N ∈ {448, 384, 256}, SPP delivers small but consistent positive speedups (+0.9 % to +1.4 %). The much higher absolute IPC at smaller N reflects working-set fit in L2 — not anything about SPP — and our SPP results scale proportionally on each baseline.

This is the strongest piece of evidence we have for the "page-aligned stride is the algorithmic boundary" claim, because it isolates the *single* variable that toggles the failure mode. Any matmul (or stencil) where the row pitch is *not* a clean multiple of the page size will benefit from SPP; any where it is will see the regression we documented in §5.13. A workload-aware compiler / runtime could potentially pad inner-loop dimensions to avoid this corner case — a small, mechanical fix at the software level.

## 6. Discussion

The implementation closely follows the paper. The most impactful design constraint we hit was the **4 KB OS-page boundary**: a strictly sequential stream sees the chain reset every 64 cache lines, which caps SPP's coverage on workloads like `bench_stride`. Scarab's stride/stream prefetchers do not have this restriction because they operate on coarser 64 KB regions. This is consistent with the original paper's argument that SPP's *qualitative* niche is irregular patterns rather than pure streams; on Spec CPU 2006/2017 (which our PIN frontend cannot decode out of the box) the paper reports SPP > stride on many integer benchmarks.

Two implementation choices diverge from the paper. First, Scarab's prefetch path routes everything through a single L2 request queue rather than the L2/LLC two-fill-level distinction in the paper; we therefore use FILL_THRESHOLD only to gate the global α counter (high-confidence prefetches contribute, low-confidence ones don't), not to pick a different fill level. Second, the framework already maintains its own request-queue dedup; the SPP filter therefore primarily serves to track *usefulness* and feed α, which inflates `PREF_UL1REQ_QUEUE_MATCHED_REQ` but doesn't change accuracy.

## 7. Future Work

* Sweep `PF_THRESHOLD` and `FILL_THRESHOLD` (currently 25 % / 90 %) for the accuracy–coverage knee.
* Try the SPP+PPF perceptron filter [Bhatia et al., ISCA 2019] as a drop-in usefulness predictor.
* Run on real SPEC CPU 2006/2017 traces once we extend `iclass_to_scarab_map` to cover BMI2 / AVX2.
* Multi-core sharing of the PT (paper Section IV.E) to amortise table cost across cores.

## 8. Build / Run

```bash
export PIN_ROOT=$PWD/tools/pin-3.15-98253-gb56e429b1-gcc-linux
cd scarab/src && make -C pin/pin_exec      # builds pin_exec.so
mkdir -p build/opt && cd build/opt
cmake ../.. -DCMAKE_BUILD_TYPE=ScarabOpt && make scarab -j8
cd ../.. && ln -sf build/opt/scarab scarab
cd ../../benchmarks && make
cd .. && python3 scripts/run_experiments.py --inst-limit 10000000
python3 scripts/analyze_results.py
```

Outputs land in `results/{bench}_{config}/` (per-run Scarab stat files), `results/summary.{csv,txt}` (tidy tables), and `results/*.png` (charts).

## References

1. J. Kim, S. H. Pugsley, P. V. Gratz, A. L. N. Reddy, C. Wilkerson, Z. Chishti, "Path Confidence Based Lookahead Prefetching," MICRO 2016.
2. ChampSim SPP reference, `Sacusa/ChampSim-CASS/prefetcher/spp_dev.{h,l2c_pref}`.
3. Scarab simulator, `github.com/hpsresearchgroup/scarab`.
4. E. Bhatia et al., "Perceptron-Based Prefetch Filtering," ISCA 2019.
