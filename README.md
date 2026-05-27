# CSE 220 Final Project — Signature Path Prefetcher (SPP) in Scarab

Implementation of **Kim et al., "Path Confidence Based Lookahead Prefetching," MICRO 2016** in the [Scarab](https://github.com/hpsresearchgroup/scarab) cycle-accurate simulator.

## Repository layout

```
docs/                       project plan, final report (md), presentation outline
benchmarks/                 5 micro-benchmarks (sequential, strided, stencil, ll, random)
scripts/                    run + analysis driver (python3)
scarab/                     submodule-style copy of the upstream Scarab simulator
  src/prefetcher/pref_spp.{h,c,param.def,param.h}    NEW -- SPP implementation
  src/prefetcher/pref_table.def                       MODIFIED -- registered "spp"
  src/prefetcher/pref_common.c                        MODIFIED -- include pref_spp.h
  src/prefetcher/pref.stat.def                        MODIFIED -- 11 SPP stat events
  src/param_files.def                                 MODIFIED -- include pref_spp.param.def
  src/ramulator/StatType.h                            FIXED -- <cstdint> for gcc-13
  src/debug/debug_print.h                             FIXED -- disasm_reg conflict
  src/CMakeLists.txt                                  FIXED -- -fcommon for gcc-13
  src/pin/pin_exec/makefile.rules                     FIXED -- -Wno-error for new gcc
  src/pin/pin_lib/decoder.cc                          FIXED -- const auto& in range-for
results/                    per-(bench,cfg) Scarab stat dumps + summary csv + plots
tools/                      Intel PIN 3.15 (downloaded, not in git)
```

The presentation is also available as a self-contained HTML slide deck at
`docs/slides.html` (open in any modern browser; reveal.js is loaded from a
CDN). Run `python3 scripts/render_slides.py` to regenerate after edits.

## Building

Requires Ubuntu (tested on 24.04), GCC 13.3, CMake ≥ 3.5, Python 3.8+ with `matplotlib`.

```bash
# 1. Get Scarab and its submodules (xed, mbuild are required; dynamorio is optional).
cd scarab && git submodule update --init --depth 1 src/deps/mbuild src/deps/xed

# 2. Drop Intel PIN 3.15 into ./tools/
wget https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz
mkdir -p tools && tar -xzf pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz -C tools/

# 3. Build the PIN-side tool and the simulator.
export PIN_ROOT=$PWD/tools/pin-3.15-98253-gb56e429b1-gcc-linux
cd scarab/src/pin/pin_exec && PIN_ROOT=$PIN_ROOT make
cd ../../build/opt 2>/dev/null || (mkdir -p ../../build/opt && cd ../../build/opt)
cmake ../.. -DCMAKE_BUILD_TYPE=ScarabOpt && make scarab -j8
cd ../../ && ln -sf build/opt/scarab scarab

# 4. Build the benchmarks.
cd ../../benchmarks && make
```

## Running

```bash
# Full experiment matrix (20M instructions per run, ~25 min total).
python3 scripts/run_experiments.py --inst-limit 20000000

# Smaller smoke test.
python3 scripts/run_experiments.py --inst-limit 5000000 --bench stride --config nopref,spp

# Analyse and plot.
python3 scripts/analyze_results.py
ls results/*.png results/summary.csv
```

## Algorithm summary

SPP is a cascade of four hardware structures:

* **Signature Table** (1×256, 12-bit signature per page) — keeps a rolling-hash fingerprint of recent intra-page deltas.
* **Pattern Table** (512×4, signed delta + 4-bit per-delta confidence + 4-bit per-set total) — histograms what delta usually follows each signature.
* **Prefetch Filter** (1024-entry quotient filter) — de-dups outstanding prefetches and feeds the global accuracy α.
* **Global History Register** (8 entries) — carries one outstanding prefetch across a 4 KB page boundary so the next page can re-seed its signature.

On each L2 demand SPP (1) updates the page's signature, (2) trains the PT with the observed delta, and (3) iteratively looks ahead, multiplying confidences and stopping when path-confidence × global-α drops below `PF_THRESHOLD = 25%`. High-confidence (≥ 90 %) prefetches mark the filter `valid`; demands that hit a `valid` filter entry bump `pf_useful`, which feeds back into α via `α = pf_useful / pf_issued`.

## Headline result

Across 5 micro-benchmarks (10 M instructions each after fast-forward):

| Benchmark | nopref IPC | stride speedup | stream speedup | **SPP speedup** |
|-|-|-|-|-|
| `stride`     | 1.814 | 1.07× | **1.20×** | 0.95× |
| `strided`    | 0.331 | 1.01× | 1.00×     | 1.00× |
| `2dstencil`  | 2.536 | 1.18× | **1.37×** | **1.07×** |
| `linkedlist` | 0.049 | **3.77×** | 3.71× | **3.47×** |
| `random`     | 0.625 | 1.00× | 1.00×     | 1.00× |

SPP achieves **3.47× speedup on pointer-chasing** and **does no harm on random access**, exactly the conservative-but-aggressive behaviour the paper aims for. See `docs/final_report.md` for full analysis.

## Citations

```
@inproceedings{kim2016spp,
  title={Path Confidence Based Lookahead Prefetching},
  author={Kim, Jinchun and Pugsley, Seth H. and Gratz, Paul V. and Reddy, A. L. Narasimha
          and Wilkerson, Chris and Chishti, Zeshan},
  booktitle={MICRO},
  year={2016}
}
```
