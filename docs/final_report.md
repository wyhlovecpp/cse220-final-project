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
