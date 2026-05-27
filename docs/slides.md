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

## Ablation: what each piece contributes

| Configuration | `linkedlist` | `2dstencil` |
|-|-|-|
| nopref                          | 1.00×     | 1.00×     |
| **spp** (full)                  | **3.47×** | **1.07×** |
| spp − lookahead (depth = 1)     | 1.56×     | 1.01×     |
| spp − GHR (no page boot)        | 3.26×     | 1.07×     |

- **Lookahead is the workhorse** — disabling it collapses the linked-list speedup from 3.47× to 1.56× (≈ 60 % of SPP's gain).
- **GHR contributes ~6 %** on linked-list, ~0 % on the stencil — its 8 entries can't track all the pages a sweeping workload touches.

---

## Parameter sweep: PF_THRESHOLD knee

| `PF_THRESHOLD` | IPC | speedup | accuracy | avg depth |
|-|-|-|-|-|
| 10 % | 0.165 | 3.39× | 100 % | 15.9 (cap) |
| **25 %** (default) | 0.169 | 3.47× | 100 % | 12.3 |
| **40 %** (knee)    | **0.174** | **3.57×** | 100 % | 8.3 |
| 60 % | 0.166 | 3.41× | 100 % | 4.5 |
| 80 % | 0.099 | 2.03× | 97 % | 1.5 |

- The **paper's default 25 % is safe but not the knee**: 40 % is mildly better on a workload where every link is 100 %-confident.
- Too aggressive (10 %) saturates the L2 queue; too conservative (80 %) collapses depth to ~1 and halves coverage.

---

## Does the knee generalise? — pf=40 on every benchmark

| Benchmark | pf=25 (default) | **pf=40 (knee)** | Δ |
|-|-|-|-|
| stride     | 1.730 | **1.757** | +1.6 % |
| strided    | 0.331 | 0.331     |  0 % |
| 2dstencil  | 2.720 | **2.751** | +1.1 % |
| linkedlist | 0.169 | **0.174** | +3.0 % |
| random     | 0.625 | 0.625     |  0 % |

- **pf=40 is never worse**, 1–3 % better on every workload where SPP issues prefetches.
- The paper's 25 % was tuned against a different L2 / MSHR — 40 % looks like a strict win for this Scarab Kaby-Lake config.

---

## MAX_DEPTH sweep — chain saturates by 8 hops

| MAX_DEPTH | IPC | observed depth |
|-|-|-|
| 1  | 0.076 | 0.7 (no lookahead) |
| 2  | 0.109 | 1.7 |
| 4  | 0.161 | 4.0 (cap) |
| **8**  | **0.176** | **7.6** (knee) |
| 16 | 0.174 | 8.2 (saturated) |
| 32 | 0.174 | 8.2 |
| 64 | 0.174 | 8.2 |

- At `pf=40`, the path-confidence threshold **terminates the chain at ≈ 8 hops**, not the cap.
- `MAX_DEPTH = 16` is slightly *worse* than `8` because of MSHR pressure on prefetches the chain would have killed anyway.
- Both knees compose: `pf=40, depth=8` is the design point for this Scarab+Kaby-Lake config.

---

## Why doesn't GHR help on 2-D stencil?

Earlier we guessed: *"GHR's 8 entries can't track all the pages a sweeping workload touches."*

We tested it — GHR_ENTRIES ∈ {8, 16, 32, 64, 128} all give **identical** IPC = 2.751 on stencil.

**First hypothesis** — encoding limit (max |delta|=63 vs ±W=±64): tested by widening to 8/10 bits → **IPC identical to 4 decimals**. Hypothesis falsified.

**Actual reason** — in-page delta is always in [-63, +63], so encoding is never binding. The lookahead chain extends *along* the row (delta=+1) one line at a time; it only generates a *cross-row* hop at the row's last line — never as a chain step. So PT never learns "+W after sig X", and GHR has no useful entry to carry. Fix would require a different algorithm (e.g. spatial streaming), not a wider encoding.

**Side-finding**: on linkedlist, bit=8 is mildly *worse* than 7 (0.169 vs 0.174). The encoding's sign-bit position perturbs PT-bucket distribution non-trivially.

---

## All prefetchers head-to-head on linkedlist

| Prefetcher | IPC | speedup |
|-|-|-|
| nopref | 0.049 | 1.00× |
| markov | 0.073 | 1.51× |
| ghb | 0.139 | 2.86× |
| stream | 0.180 | 3.71× |
| **SPP @ pf=40, d=8** | **0.176** | **3.59×** |
| stride | 0.183 | 3.77× |

SPP sits in the top tier — within 4 % of the best (stride) and ahead of every other history-based prefetcher.

---

## The composed knee — `pf=40, d=8` on every benchmark

| Benchmark | Paper default<br>(pf=25, d=16) | **Both knees**<br>(pf=40, d=8) | improvement |
|-|-|-|-|
| stride     | 1.730 | **1.773** | +2.5 % |
| strided    | 0.331 | 0.331     |   0    |
| 2dstencil  | 2.720 | **2.773** | +1.9 % |
| linkedlist | 0.169 | **0.176** | +4.1 % |
| random     | 0.625 | 0.625     |   0    |

**The two knees compose** — `pf=40, d=8` is the best SPP we found on every benchmark, beating the paper's default by 1.9 – 4.1 % with no regression anywhere.

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
