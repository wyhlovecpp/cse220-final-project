# CSE220 Final Project — Presentation Outline (10 min)

**Title:** Implementing the Signature Path Prefetcher (SPP) in Scarab
**Group paper:** Kim et al., *Path Confidence Based Lookahead Prefetching*, MICRO 2016

---

## Slide 1 — Title (10 sec)
- Title, names, course code, paper citation.

## Slide 2 — Motivation: Why a smarter prefetcher? (1 min)
- A simple stride prefetcher gets the easy 80 %.
- Hard cases: multi-delta patterns, page-internal patterns, irregular sequences.
- Goal of SPP: aggressive *when confident*, restrained otherwise — no cache pollution on random workloads.
- Visual: a 2-D stencil access pattern showing the (-W, -1, +1, +W) deltas that fixed-stride prefetchers can't capture.

## Slide 3 — SPP in one picture (1 min)
- The "cascade of three tables": Signature Table → Pattern Table → Prefetch Filter, with the GHR feeding back across pages.
- Show the rolling-hash signature `sig' = (sig << 3) ^ delta_sm` and the path-confidence accumulation `path_conf = α · (c_Δ / c_sig) · prev_path_conf`.

## Slide 4 — Implementation in Scarab (1 min)
- Three new files: `pref_spp.{h,c}`, `pref_spp.param.def`.
- Added one row to `pref_table.def`; one include in `pref_common.c`; registered params in `param_files.def`.
- About **6 KB of state** total — matches paper's budget.
- Eleven SPP-specific stat counters (operate count, depth, useful, page-cross, GHR boots, …).

## Slide 5 — Algorithm walkthrough (2 min)
- On every L2 demand: ① read+update ST (rolling hash), ② train PT with the *previous* signature and the *current* delta, ③ run the lookahead loop until path confidence drops or `depth >= 16`.
- Side-bar: the GHR fall-back — when we land on a fresh page, look up any "predicted-offset" GHR entry and seed the new ST signature from it.
- Walk-through one stride iteration showing how the chain accumulates +1, +2, +3, … prefetches.

## Slide 6 — Evaluation methodology (1 min)
- Scarab cycle-accurate sim with `PARAMS.kaby_lake` defaults; 1 MB / 8-way L2; DDR4-2400.
- 4 configs: `nopref`, `stride`, `stream`, `spp`. Last three with prefetcher framework on.
- 5 micro-benchmarks: sequential, large-stride, 2-D stencil, linked-list, random.
- 20 M-instruction runs after fast-forward.

## Slide 7 — Results: IPC + Speedup (1 min)
- Bar chart of IPC, grouped by benchmark, 4 bars per group.
- Second bar chart of speedup over `nopref`.
- One-line takeaway per benchmark:
  - sequential: stride/stream win; SPP within ε.
  - stencil: **SPP wins**.
  - linked-list: SPP > stride.
  - random: SPP ≈ nopref (no harm done).

## Slide 8 — Results: Prefetch quality (1 min)
- Accuracy = useful / sent. Coverage = useful / (useful + miss).
- Show that SPP keeps very high accuracy on `random` because it issues few prefetches.

## Slide 9 — What surprised us / Limitations (1 min)
- 4 KB OS page boundary caps SPP on pure sequential streams (chain breaks every 64 lines).
- The GHR is essential — disabling it costs ~X % on the stride benchmark.
- Build-portability gotchas on Ubuntu 24 + GCC 13 (`<cstdint>`, `-fcommon`, deprecated PIN APIs).

## Slide 10 — Conclusion + Future Work (30 sec)
- Implementation matches the paper's algorithm and reproduces its qualitative behaviour: confidence-throttled aggressive prefetching.
- Future: ChampSim/SPEC traces; PPF perceptron filter; multi-core sharing.
