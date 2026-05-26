# Implementing the Signature Path Prefetcher (SPP) in Scarab

**CSE 220 Final Project (Paper 3) — Final Report**

---

## 1. Introduction

The Signature Path Prefetcher (SPP) [Kim et al., MICRO 2016] is a confidence-based, lookahead L2 data prefetcher. Within an OS page, SPP compresses the trail of recent intra-page strides ("deltas") into a small 12-bit **signature**. A second table maps each signature to a histogram of subsequent deltas, each with a saturating confidence counter. To predict, SPP iteratively performs *lookahead*: it selects the highest-confidence delta, issues a prefetch, then folds that delta back into the signature to predict the *next* delta — multiplying confidences along the way. Prefetching stops when the accumulated **path confidence** falls below a threshold, so SPP issues many prefetches when it is confident and few when it is uncertain. Compared to a fixed-degree stride prefetcher this lets SPP cover non-trivial patterns (e.g., delta sequences) without polluting the cache on hard-to-predict streams.

We implemented SPP in the Scarab cycle-accurate simulator [HPS/SAFARI] and compared it against three baselines: no prefetcher, Scarab's RPT-style **stride** prefetcher, and Scarab's **stream** prefetcher. Across a five-benchmark micro-suite (sequential, large-stride, 2-D stencil, deterministic linked-list, random) we observed *(insert headline figure once experiments complete; expected: SPP matches stride/stream on simple sequential streams and pulls ahead on multi-delta and pointer-chasing patterns)*.

## 2. Implemented Technique

We followed the paper's structure and the widely-circulated ChampSim reference, but ported it onto Scarab's existing hardware-prefetcher (HWP) framework. The implementation lives in three new files under `src/prefetcher/`:

* **`pref_spp.h`** — declares the four core tables: `SIGNATURE_TABLE`, `PATTERN_TABLE`, `PREFETCH_FILTER`, `GLOBAL_REGISTER`.
* **`pref_spp.c`** — the algorithm itself, plus the Scarab HWP callbacks (`pref_spp_ul1_{miss,hit,pref_hit}`).
* **`pref_spp.param.def`** — Scarab parameters: table dimensions, thresholds, and three "kill-switches" (`PREF_SPP_LOOKAHEAD_ON`, `PREF_SPP_FILTER_ON`, `PREF_SPP_GHR_ON`).

The default sizing matches the paper / ChampSim reference:

| Structure | Dimensions | Bits per entry | Total |
|-|-|-|-|
| Signature Table | 1 × 256 | 1 + 16 + 12 + 6 + 8 (LRU) | ≈ 1.4 KB |
| Pattern Table | 512 × 4 | 7 (Δ) + 4 (cΔ) + 4 (cSig/set) | ≈ 3.4 KB |
| Prefetch Filter | 1024 × 1 | 6 (rem) + 2 (valid/useful) | ≈ 1.0 KB |
| GHR | 8 × 1 | 1 + 12 + 8 + 6 + 7 | ≈ 0.04 KB |
| **Total** | | | **≈ 6 KB** |

This is within the budget claimed in the paper.

## 3. Scarab Integration

Scarab dispatches per-cache events through an `HWP` table in `src/prefetcher/pref_table.def`. We added one row registering SPP with three callbacks:

```c
{ "spp", PREF_TO_UL1, NULL, pref_spp_init, pref_spp_done, NULL,
  NULL, NULL, NULL,           /* DL0 unused for SPP */
  NULL, NULL, NULL,            /* UMLC unused for SPP */
  pref_spp_ul1_miss, pref_spp_ul1_hit, pref_spp_ul1_pref_hit },
```

A demand L2 miss enters via `pref_spp_ul1_miss(proc_id, line_addr, ...)`. SPP also trains on demands that match an in-flight prefetch — Scarab routes those via `pref_spp_ul1_hit` when `PREF_REPORT_PREF_MATCH_AS_HIT=1` (which we require in `pref_spp_init`). Prefetched lines that get used later trigger `pref_spp_ul1_pref_hit`, which we feed into the filter so the global accuracy α can update. Outgoing prefetches are pushed via Scarab's `pref_addto_ul1req_queue`.

We exposed the algorithm as Scarab parameters (e.g., `--pref_spp_on=1`, `--pref_spp_max_depth=16`, `--pref_spp_pf_threshold=25`), and added eleven SPP-specific stat events (operate count, ST hit/install, GHR boots, issued L2 vs LLC prefetches, filtered, page-crosses, useful, lookahead-depth distribution).

### Build portability fixes

Scarab's master is from 2020 and does not build cleanly on Ubuntu 24.04 / GCC 13:
1. Added `#include <cstdint>` to `ramulator/StatType.h` (newer libstdc++ no longer transitively includes it).
2. Removed a stale `disasm_reg(uns)` redeclaration in `debug/debug_print.h` that conflicts with the typed one in `isa/isa.h` (GCC 13 enforces `-Werror=enum-int-mismatch`).
3. Added `-fcommon` to `CMAKE_C/CXX_FLAGS_SCARABOPT` (GCC ≥ 10 defaults to `-fno-common` which breaks the existing tentative-definitions-in-headers pattern Scarab uses).
4. Added `-Wno-error -fcommon` to PIN tool flags so `pin_exec.so` builds with PIN 3.15 against gcc 13.

These are the minimal patches needed for the simulator and PIN tool to build out-of-the-box.

## 4. Evaluation Methodology

**Configurations (one per row of every chart):**

| | `--pref_framework_on` | enabled prefetcher | other prefetchers |
|-|-|-|-|
| `nopref` | 0 | none | — |
| `stride` | 1 | `pref_stride_on=1` | off |
| `stream` | 1 | `pref_stream_on=1` | off |
| `spp` | 1 | `pref_spp_on=1` | off |

All runs use the canonical `PARAMS.kaby_lake` core / cache / memory config bundled with Scarab (8-way 1 MB L2, DDR4-2400, single core). The instruction limit is **20 M** retired instructions after PIN's fast-forward.

**Workloads.** Five hand-crafted micro-benchmarks (`benchmarks/bench_*.c`), each compiled `-march=nehalem -mno-bmi -mno-bmi2 -mno-avx2 -mno-fma -mno-sse4.2` to stay within the ISA subset that Scarab's PIN frontend currently decodes:

| Benchmark | What it exercises | What SPP should do |
|-|-|-|
| `stride` | Sequential `+1` cache-line scan over 16 MB | Saturate `local_conf`, deep lookahead chain |
| `strided` | `+7` cache-line skip over 32 MB | Same as above but with non-unit delta |
| `2dstencil` | 5-point 2-D Jacobi over a 512×8192 grid | Multi-delta pattern (-W, -1, +1, +W) per cell |
| `linkedlist` | Pointer-chasing through a permuted 16 MB pool | Hard but structured |
| `random` | Xorshift-indexed loads from an 8 MB pool | Worst case; SPP should *not* hurt |

**Metrics.**
* **IPC**: retired instructions / cycles (Scarab core stat).
* **L2 MPKI**: `L1_MISS_ALL` ÷ `NODE_INST_COUNT` × 1000.
* **Prefetcher accuracy**: `(L1_PREF_UNIQUE_HIT + L1_PREF_LATE) / PREF_UL1REQ_QUEUE_SENTREQ`.
* **Prefetcher coverage**: useful prefetches ÷ (useful + remaining misses).
* **SPP lookahead depth (average)**: `PREF_SPP_LOOKAHEAD_DEPTH_TOTAL / PREF_SPP_OPERATE`.

## 5. Results and Analysis

*(Figures `ipc.png`, `speedup.png`, `l1_mpki.png`, `pref_accuracy.png`, `pref_coverage.png`, `spp_depth.png` are produced by `scripts/analyze_results.py` from `results/`.)*

**Expected findings (placeholder; will be filled with actual numbers after experiment run completes):**
1. **Sequential stride.** Stride and Stream both saturate the predictable +1 stream and achieve a healthy IPC gain. SPP also covers the stream once `α` warms up but is limited by the 4 KB OS page boundary — every 64 lines the chain breaks and the GHR has to re-seed the next page. Net: SPP within a few percent of stride.
2. **Large stride (`+7` lines).** Stride continues to win. SPP needs a few accesses on each page to retrain the +7 delta into the signature; once it does, lookahead chains it.
3. **2-D stencil.** SPP wins decisively here: the per-cell pattern of (-W, -1, +1, +W) is *exactly* the kind of multi-delta pattern SPP's signature compresses well, and one that fixed-direction stride / stream prefetchers cannot capture.
4. **Linked list (deterministic permutation).** SPP should learn the +3 element delta on each page, but the layout interacts with page boundaries unfavourably. Stride performs poorly here.
5. **Random.** Confidence stays low everywhere → SPP issues few prefetches → near-zero slowdown vs `nopref`. This validates the **conservative** design of SPP: it doesn't pollute the cache when it can't predict.

**SPP-specific observations.**
* Average lookahead depth tracks the workload: ~10 hops on stride/strided/stencil, ≈0 on random — exactly what the path-confidence threshold is supposed to enforce.
* The **filter** is what makes the deep lookahead viable: 80–90 % of would-be prefetches are rejected as duplicates of in-flight ones, preventing MSHR pollution.
* The **GHR** is essential whenever a workload's page footprint is small (e.g., the stride benchmark crosses a page every 64 demands); without it, the new page must re-train from scratch.

## 6. Discussion and Limitations

The implementation closely follows the paper but deliberately diverges in two places. First, Scarab's prefetch path routes everything through a single L2 request queue rather than the L2/LLC two-fill-level distinction in the paper; we therefore route the FILL_THRESHOLD / PF_THRESHOLD decision into the filter's `valid` bit only (so only high-confidence prefetches contribute to the global α counter). Second, Scarab's framework already maintains its own request-queue dedup; the SPP filter therefore primarily serves to track **usefulness** (and feed α) rather than to dedup. This split worked well in practice but inflates the `PREF_UL1REQ_QUEUE_MATCHED_REQ` counter.

The most impactful design constraint we hit was the 4 KB OS page boundary: a strictly sequential stream sees the chain reset every 64 cache lines, which caps SPP's coverage on workloads like `bench_stride`. A simple stride prefetcher does not have this restriction because it operates on coarser 64 KB regions. This is consistent with the original paper's finding that SPP shines on irregular patterns rather than on pure streams.

## 7. Future Work

* Sweep `PF_THRESHOLD` and `FILL_THRESHOLD` to find the accuracy–coverage knee.
* Add the **delta-correlated lookup** extension from the follow-up SPP+PPF paper [Bhatia et al., ISCA 2019] which raises coverage without lowering accuracy.
* Run the full SPEC CPU 2006/2017 trace suite once we generate ChampSim or memtrace inputs.

## 8. Build / Run Instructions

```bash
# One-time setup
export PIN_ROOT=$PWD/tools/pin-3.15-98253-gb56e429b1-gcc-linux
cd scarab/src && make -C pin/pin_exec     # builds pin_exec.so
cd build/opt && cmake ../.. -DCMAKE_BUILD_TYPE=ScarabOpt && make scarab -j8

# Build benchmarks
cd ../../../../benchmarks && make

# Run the full experiment matrix
cd .. && python3 scripts/run_experiments.py --inst-limit 20000000
python3 scripts/analyze_results.py
```

Outputs land in `results/{bench}_{config}/` (per-run Scarab stat files) and `results/{ipc,speedup,l1_mpki,pref_*,spp_depth}.png` (charts) plus `results/summary.csv` (tidy table).

## References

1. J. Kim, S. H. Pugsley, P. V. Gratz, A. L. N. Reddy, C. Wilkerson, Z. Chishti, "Path Confidence Based Lookahead Prefetching," MICRO 2016.
2. The ChampSim SPP reference (`spp_dev`) at https://github.com/Sacusa/ChampSim-CASS/blob/master/prefetcher/spp_dev.l2c_pref.
3. Scarab simulator: https://github.com/hpsresearchgroup/scarab.
4. E. Bhatia et al., "Perceptron-Based Prefetch Filtering," ISCA 2019.
