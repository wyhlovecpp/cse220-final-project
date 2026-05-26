<!--
SPP slide deck. Convert to PDF / HTML with marp:

  npx @marp-team/marp-cli@latest docs/slides.md -o docs/slides.pdf
-->

---
marp: true
theme: default
paginate: true
size: 16:9
header: "CSE 220 Final Project — SPP in Scarab"
footer: "Paper 3: Kim et al., MICRO 2016"
---

# Implementing the Signature Path Prefetcher (SPP) in Scarab
**CSE 220 Final Project — Paper 3**

Kim, Pugsley, Gratz, Reddy, Wilkerson, Chishti
*"Path Confidence Based Lookahead Prefetching,"* MICRO 2016

---

## Why a smarter prefetcher?

- Stride / stream prefetchers nail the **easy 80 %** — pure sequential or fixed-stride streams.
- They give up on multi-delta, page-internal, or irregular patterns.
- **SPP's pitch**: aggressive *when confident*, restrained otherwise, so no cache pollution on hard workloads.

A 2-D stencil's per-cell deltas (−W, −1, +1, +W) defeat fixed stride/stream prefetchers, but the *sequence* is exactly the signature SPP compresses.

---

## SPP in one picture

```text
            +---------+   +--------+
demand --> | ST     | -> | PT      | -> path-confidence × α
addr        | sig 12b|   |  Δ + cΔ|        threshold = 25 %
            +---------+   +--------+               |
                  ^             |                  v
                  |             |              issue prefetch
                  |             |                  |
                  |  page-cross |                  v
                  +-------------+              [Filter (1024)] -> queue
                  GHR (8 entries)              tracks usefulness
```

`sig' = (sig ≪ 3) ⊕ Δ`  (3-bit shift × 12-bit sig × 7-bit Δ_sm)

`path_conf = α · (c_Δ / c_sig) · prev_conf`

---

## Implementation in Scarab

- **3 new files** under `src/prefetcher/`:
  - `pref_spp.h` (data structures)
  - `pref_spp.c` (~600 LoC algorithm + lookahead loop)
  - `pref_spp.param.def` (15 knobs)
- **1 row** added to `pref_table.def`; **1** include in `pref_common.c`.
- **20** SPP-specific `STAT_EVENT`s (operate, ST hit, GHR boot, …).
- Total state: **≈ 6 KB** (matches paper).

```c
{ "spp", PREF_TO_UL1, NULL, pref_spp_init, pref_spp_done, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL,
  pref_spp_ul1_miss, pref_spp_ul1_hit, pref_spp_ul1_pref_hit },
```

---

## Algorithm walkthrough (per L2 demand)

1. `ST.read_and_update_sig(page, offset)` → `(last_sig, curr_sig, Δ)`
2. `Filter.check(addr, DEMAND)` → bump α counter if prefetched
3. `if last_sig: PT.update_pattern(last_sig, Δ)`
4. **Lookahead loop**, up to `MAX_DEPTH = 16` hops:
   ```c
   for each Δ_w in PT[curr_sig]:
       path_conf = depth==0 ? local_conf : α·(c_Δ/c_sig)·prev_conf
       if path_conf >= 25%: enqueue prefetch
   advance: curr_sig = (curr_sig ≪ 3) ⊕ Δ_chosen_sm
   ```
5. Cross-page Δ ⇒ write GHR entry; next-page demand re-seeds ST from GHR.

---

## Two bugs we hit while bringing this up

- **Per-iteration scratch queue.** ChampSim grows one `pf_q` over the whole `do { ... }` loop; with multi-Δ patterns + `MAX_DEPTH = 16` that overflows. We allocate `PT_WAY + 1` slots and reset every outer iteration.
- **Stat-event OOB.** Originally indexed `STAT_EVENT(0, DEPTH_MAX + i)` for `i = 0..9` — but `DEPTH_MAX` is the **last** stat in the entire `stat_files.def` chain, so those writes ran past `global_stat_array`. Replaced with ten explicit `DEPTH_0..DEPTH_8/GE9` stats.

Both manifested as SIGSEGV on `linkedlist` / `strided` at lookahead depth ≥ 4.

---

## Evaluation methodology

- **Scarab** cycle-accurate, `PARAMS.kaby_lake` (32 KB L1d, **1 MB 8-way L2**, DDR4-2400).
- **4 configs**: `nopref`, `stride`, `stream`, `spp`.
- **5 micro-benchmarks**: sequential, large-stride, 2-D stencil, pointer-chase, random.
- **10 M-instruction** runs after PIN fast-forward.
- Metrics: IPC, L2 MPKI, prefetcher accuracy / coverage, SPP lookahead depth.

---

## Headline results: speedup over no-prefetcher

| Benchmark | stride | stream | **SPP** |
|-|-|-|-|
| `stride` (sequential) | 1.07× | **1.20×** | 0.95× |
| `strided` (+7 lines)  | 1.01× | 1.00×     | 1.00× |
| `2dstencil`           | 1.18× | **1.37×** | **1.07×** |
| `linkedlist`          | **3.77×** | 3.71× | **3.47×** |
| `random`              | 1.00× | 1.00×     | 1.00× |

**SPP shines on `linkedlist`** (3.47× — pointer-chasing with intra-page structure)
**SPP does no harm on `random`** (0 prefetches issued — confidence gate works)

---

## What surprised us

- **4 KB OS-page boundary** caps SPP on pure sequential streams (chain breaks every 64 lines). Stride/stream use coarser 64 KB regions and don't have this issue.
- On `stride`, SPP issues 303 K prefetches with **97 % accuracy**, but **90 % are *late*** — SPP arrives just-in-time while stream pre-stages farther ahead.
- On `linkedlist`, **average lookahead depth is 12 hops**; on `random` it is **0** — exactly the throttling the paper aimed for.
- Scarab on Ubuntu 24 / GCC 13 needed five small portability patches (`<cstdint>`, `-fcommon`, deprecated PIN APIs, …).

---

## Conclusion + Future Work

- **Implemented the full paper algorithm** (ST + PT + Filter + GHR + path-confidence lookahead).
- **Reproduced the qualitative behaviour**: aggressive when confident, quiet otherwise, decisive win on linked-list, no harm on random.
- **Future**:
  - SPEC CPU traces once we extend Scarab's BMI2/AVX2 decoder.
  - SPP+PPF perceptron filter [Bhatia ISCA'19] for usefulness prediction.
  - Multi-core PT sharing.

**Repo**: https://github.com/wyhlovecpp/cse220-final-project — see `README.md` for build + run.

---

## Q & A

Thanks!
