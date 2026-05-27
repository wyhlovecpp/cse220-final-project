/* Copyright 2026 CSE220 Final Project Group
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction. Provided "AS IS" without warranty.
 */

/***************************************************************************************
 * File         : pref_spp.c
 * Description  : Signature Path Prefetcher (SPP) -- MICRO 2016, Kim et al.
 *
 * The implementation follows the paper (Sections III.A--III.E) and the
 * widely-circulated ChampSim reference. It is ported to Scarab's HWP
 * framework: training happens on L2 (UL1) misses and on demand requests that
 * match a pending prefetch (treated as misses via PREF_REPORT_PREF_MATCH_AS_*).
 *
 * Major differences from the ChampSim port:
 *   * No multi-level (L2 vs LLC) fill split -- Scarab routes everything via
 *     the UL1 request queue. We still compute the FILL_THRESHOLD-vs-PF_THRESHOLD
 *     decision and only count FILL-level prefetches against the global
 *     accuracy counters, mimicking the paper's selectivity.
 *   * All sizes, bit widths and thresholds are exposed as Scarab parameters
 *     (see pref_spp.param.def).
 ***************************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"

#include "globals/assert.h"
#include "globals/utils.h"
#include "op.h"

#include "core.param.h"
#include "dcache_stage.h"
#include "debug/debug.param.h"
#include "general.param.h"
#include "libs/cache_lib.h"
#include "libs/hash_lib.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"
#include "prefetcher/pref_common.h"
#include "prefetcher/pref_spp.h"
#include "prefetcher/pref_spp.param.h"
#include "statistics.h"

#define DEBUG(args...) _DEBUG(DEBUG_PREF_SPP, ##args)

/* ============================================================ */
/* Global state                                                  */
/* ============================================================ */

static Pref_SPP* spp = NULL;

/* ============================================================ */
/* Helpers                                                       */
/* ============================================================ */

/* Robert Jenkins' 32-bit mix + Knuth's multiplicative hash -- same hash
 * used in the ChampSim reference. */
static inline uns64 spp_hash64(uns64 key) {
  key += (key << 12);
  key ^= (key >> 22);
  key += (key << 4);
  key ^= (key >> 9);
  key += (key << 10);
  key ^= (key >> 2);
  key += (key << 7);
  key ^= (key >> 12);
  key = (key >> 3) * 2654435761ULL;
  return key;
}

/* Encode a signed delta into the SIG_DELTA_BIT-wide sign-magnitude form used
 * in the paper (top bit is the sign, the rest holds |delta|). */
static inline uns spp_encode_sig_delta(int delta) {
  if(delta < 0) {
    return ((uns)(-delta)) + spp->sig_delta_sign_bit;
  }
  return (uns)delta;
}

/* ============================================================ */
/* Signature Table                                               */
/* ============================================================ */

/* read_and_update_sig follows the paper / ChampSim reference closely:
 *   * Case 1: page hit -> read previous signature, compute delta from the
 *     last offset, mix delta into the rolling signature, update last_offset.
 *   * Case 2: no entry yet -> install at an invalid way with sig=0.
 *   * Case 3: all ways valid -> LRU victim, sig=0 on the new entry.
 *
 * On a Case-2 / Case-3 install we additionally consult the GHR (Section III.D)
 * so that a page-crossing prefetch path can carry over. */
static void spp_st_read_and_update(Addr page, uns page_offset,
                                   uns* last_sig, uns* curr_sig, int* delta) {
  const uns st_set = PREF_SPP_ST_SET;
  const uns st_way = PREF_SPP_ST_WAY;
  uns set          = (uns)(spp_hash64(page) % st_set);
  uns partial_page = (uns)(page & spp->st_tag_mask);
  Spp_ST_Entry* row = spp->st[set];
  uns match         = st_way;
  Flag st_hit       = FALSE;
  int sig_delta     = 0;

  *last_sig = 0;
  *curr_sig = 0;
  *delta    = 0;

  /* Case 1: hit. */
  for(uns w = 0; w < st_way; w++) {
    if(row[w].valid && row[w].tag == partial_page) {
      match     = w;
      *last_sig = row[w].sig;
      *delta    = (int)page_offset - (int)row[w].last_offset;
      if(*delta) {
        sig_delta = spp_encode_sig_delta(*delta);
        row[w].sig =
          ((row[w].sig << PREF_SPP_SIG_SHIFT) ^ sig_delta) & spp->sig_mask;
        *curr_sig          = row[w].sig;
        row[w].last_offset = page_offset;
      } else {
        /* same line as before; don't pollute correlations. */
        *last_sig = 0;
      }
      st_hit = TRUE;
      break;
    }
  }

  /* Case 2: an invalid way is available. */
  if(match == st_way) {
    for(uns w = 0; w < st_way; w++) {
      if(!row[w].valid) {
        match              = w;
        row[w].valid       = TRUE;
        row[w].tag         = partial_page;
        row[w].sig         = 0;
        row[w].last_offset = page_offset;
        *curr_sig          = 0;
        break;
      }
    }
  }

  /* Case 3: evict LRU. */
  if(match == st_way) {
    for(uns w = 0; w < st_way; w++) {
      if(row[w].lru == st_way - 1) {
        match              = w;
        row[w].tag         = partial_page;
        row[w].sig         = 0;
        row[w].last_offset = page_offset;
        *curr_sig          = 0;
        break;
      }
    }
    ASSERTM(0, match != st_way, "[SPP-ST] LRU victim not found\n");
  }

  if(st_hit) STAT_EVENT(0, PREF_SPP_ST_HIT);
  else       STAT_EVENT(0, PREF_SPP_ST_MISS_INSTALL);

  /* GHR fall-back (Section III.D): if we just installed a new page, prime
   * its signature from any GHR entry that predicted this exact landing
   * offset on a previous page-crossing path. */
  if(PREF_SPP_GHR_ON && !st_hit) {
    uns best     = PREF_SPP_GHR_ENTRIES;
    uns best_cnf = 0;
    for(uns i = 0; i < PREF_SPP_GHR_ENTRIES; i++) {
      if(spp->ghr[i].valid && spp->ghr[i].offset == page_offset &&
         spp->ghr[i].confidence > best_cnf) {
        best     = i;
        best_cnf = spp->ghr[i].confidence;
      }
    }
    if(best < PREF_SPP_GHR_ENTRIES) {
      STAT_EVENT(0, PREF_SPP_GHR_BOOT);
      sig_delta = spp_encode_sig_delta(spp->ghr[best].delta);
      row[match].sig =
        ((spp->ghr[best].sig << PREF_SPP_SIG_SHIFT) ^ sig_delta) &
        spp->sig_mask;
      *curr_sig = row[match].sig;
    }
  }

  /* Update LRU: promote match to MRU. */
  uns matched_lru = row[match].lru;
  for(uns w = 0; w < st_way; w++) {
    if(row[w].lru < matched_lru) {
      row[w].lru++;
      ASSERTM(0, row[w].lru < st_way, "[SPP-ST] LRU overflow\n");
    }
  }
  row[match].lru = 0;
}

/* ============================================================ */
/* Pattern Table                                                  */
/* ============================================================ */

static void spp_pt_update_pattern(uns last_sig, int curr_delta) {
  const uns pt_set     = PREF_SPP_PT_SET;
  const uns pt_way     = PREF_SPP_PT_WAY;
  const uns c_sig_max  = (1u << PREF_SPP_C_SIG_BIT) - 1u;
  uns set              = (uns)(spp_hash64(last_sig) % pt_set);
  Spp_PT_Set* pt_row   = &spp->pt[set];
  Spp_PT_Way* ways     = pt_row->ways;
  uns match            = pt_way;

  /* Hit on matching delta -> bump per-delta confidence. */
  for(uns w = 0; w < pt_way; w++) {
    if(ways[w].delta == curr_delta) {
      match = w;
      if(ways[w].c_delta < (1u << PREF_SPP_C_DELTA_BIT) - 1u)
        ways[w].c_delta++;
      pt_row->c_sig++;
      if(pt_row->c_sig > c_sig_max) {
        for(uns w2 = 0; w2 < pt_way; w2++) ways[w2].c_delta >>= 1;
        pt_row->c_sig >>= 1;
      }
      return;
    }
  }

  /* Miss -> install in slot with the smallest c_delta. */
  uns victim     = 0;
  uns min_c      = ways[0].c_delta;
  for(uns w = 1; w < pt_way; w++) {
    if(ways[w].c_delta < min_c) {
      victim = w;
      min_c  = ways[w].c_delta;
    }
  }
  ways[victim].delta   = curr_delta;
  ways[victim].c_delta = 0;
  pt_row->c_sig++;
  if(pt_row->c_sig > c_sig_max) {
    for(uns w2 = 0; w2 < pt_way; w2++) ways[w2].c_delta >>= 1;
    pt_row->c_sig >>= 1;
  }
}

/* read_pattern enumerates each PT way at the current signature, computes a
 * path-confidence-weighted prefetch confidence, and selects the highest
 * confidence way to drive the next lookahead iteration. Mirrors the paper's
 * Equation 3 (alpha-weighted path confidence after the first hop). */
static void spp_pt_read_pattern(uns curr_sig, int* delta_q, uns* conf_q,
                                uns* lookahead_way, uns* lookahead_conf,
                                uns* pf_q_tail, uns depth) {
  const uns pt_set = PREF_SPP_PT_SET;
  const uns pt_way = PREF_SPP_PT_WAY;
  uns set          = (uns)(spp_hash64(curr_sig) % pt_set);
  Spp_PT_Set* row  = &spp->pt[set];
  uns max_conf     = 0;
  *lookahead_way   = pt_way;

  if(row->c_sig == 0) {
    conf_q[*pf_q_tail] = 0;
    return;
  }

  for(uns w = 0; w < pt_way; w++) {
    uns local_conf = (uns)((100ULL * row->ways[w].c_delta) / row->c_sig);
    uns path_conf;
    if(depth == 0) {
      path_conf = local_conf;
    } else {
      /* alpha * (c_delta / c_sig) * lookahead_conf -- integer-only. */
      path_conf = (uns)((uns64)spp->global_accuracy_pct * row->ways[w].c_delta /
                       row->c_sig * (*lookahead_conf) / 100ULL);
    }
    if(path_conf >= PREF_SPP_PF_THRESHOLD) {
      conf_q[*pf_q_tail]  = path_conf;
      delta_q[*pf_q_tail] = row->ways[w].delta;
      if(path_conf > max_conf) {
        *lookahead_way = w;
        max_conf       = path_conf;
      }
      (*pf_q_tail)++;
    }
  }
  *lookahead_conf = max_conf;
}

/* ============================================================ */
/* Prefetch Filter                                                */
/* ============================================================ */

/* Returns TRUE if the caller should issue this prefetch; FALSE if it would
 * be a duplicate (already in-flight / installed). The filter is also used to
 * track usefulness on demand hits and reset state on evictions. */
static Flag spp_filter_check(Addr addr, Spp_Filter_Req req) {
  if(!PREF_SPP_FILTER_ON) return TRUE;

  uns64 cache_line = addr >> LOG2(DCACHE_LINE_SIZE);
  uns64 h          = spp_hash64(cache_line);
  uns   quotient   = (uns)((h >> PREF_SPP_REMAINDER_BIT) &
                         ((1ULL << PREF_SPP_QUOTIENT_BIT) - 1ULL));
  uns   remainder  = (uns)(h & ((1ULL << PREF_SPP_REMAINDER_BIT) - 1ULL));
  Spp_Filter_Entry* e = &spp->filter[quotient];

  switch(req) {
    case SPP_FILTER_L2_PREFETCH:
      if((e->valid || e->useful) && e->remainder_tag == remainder) {
        return FALSE;
      }
      e->valid         = TRUE;
      e->useful        = FALSE;
      e->remainder_tag = remainder;
      return TRUE;

    case SPP_FILTER_LLC_PREFETCH:
      /* Lower confidence prefetch -- mirror paper: don't lock filter slot so
       * a later high-confidence prefetch can still install. */
      if((e->valid || e->useful) && e->remainder_tag == remainder)
        return FALSE;
      return TRUE;

    case SPP_FILTER_DEMAND:
      if(e->remainder_tag == remainder && !e->useful) {
        e->useful = TRUE;
        if(e->valid) {
          STAT_EVENT(0, PREF_SPP_PF_USEFUL);
          spp->pf_useful++;
          if(spp->pf_useful > spp->global_counter_max) {
            spp->pf_useful >>= 1;
            spp->pf_issued >>= 1;
          }
        }
      }
      return TRUE;

    case SPP_FILTER_EVICT:
      if(e->valid && !e->useful && spp->pf_useful)
        spp->pf_useful--;
      e->valid         = FALSE;
      e->useful        = FALSE;
      e->remainder_tag = 0;
      return TRUE;
  }
  return TRUE;
}

/* ============================================================ */
/* Global History Register                                         */
/* ============================================================ */

static void spp_ghr_update(uns pf_sig, uns pf_conf, uns pf_offset,
                           int pf_delta) {
  if(!PREF_SPP_GHR_ON) return;
  uns victim = PREF_SPP_GHR_ENTRIES;
  uns min_c  = (uns)-1;

  for(uns i = 0; i < PREF_SPP_GHR_ENTRIES; i++) {
    /* Match-and-refresh: keep one entry per offset. */
    if(spp->ghr[i].valid && spp->ghr[i].offset == pf_offset) {
      spp->ghr[i].sig        = pf_sig;
      spp->ghr[i].confidence = pf_conf;
      spp->ghr[i].delta      = pf_delta;
      return;
    }
    /* Track victim with the lowest confidence. */
    if(spp->ghr[i].confidence < min_c) {
      min_c  = spp->ghr[i].confidence;
      victim = i;
    }
  }
  ASSERTM(0, victim < PREF_SPP_GHR_ENTRIES, "[SPP-GHR] no victim\n");
  spp->ghr[victim].valid      = TRUE;
  spp->ghr[victim].sig        = pf_sig;
  spp->ghr[victim].confidence = pf_conf;
  spp->ghr[victim].offset     = pf_offset;
  spp->ghr[victim].delta      = pf_delta;
}

/* ============================================================ */
/* Main prefetch driver                                            */
/* ============================================================ */

/* This is the SPP "operate" function (paper Section III, ChampSim
 * l2c_prefetcher_operate). Called on every UL1 demand (miss or matched hit). */
static void spp_operate(uns8 proc_id, Addr lineAddr) {
  ASSERTM(proc_id, spp != NULL, "[SPP] not initialised\n");

  const uns log2_line = LOG2(DCACHE_LINE_SIZE);
  const uns log2_page = LOG2(VA_PAGE_SIZE_BYTES);
  const uns offs_mask = (VA_PAGE_SIZE_BYTES / DCACHE_LINE_SIZE) - 1u;

  Addr addr        = lineAddr & ~((Addr)(DCACHE_LINE_SIZE - 1));
  Addr page        = addr >> log2_page;
  uns  page_offset = (uns)((addr >> log2_line) & offs_mask);
  uns  last_sig    = 0;
  uns  curr_sig    = 0;
  int  delta       = 0;

  /* Update global accuracy alpha (paper Eq. 3). */
  spp->global_accuracy_pct =
    spp->pf_issued ? (uns)((100ULL * spp->pf_useful) / spp->pf_issued) : 0;

  STAT_EVENT(proc_id, PREF_SPP_OPERATE);

  /* Stage 1: read+update signature. */
  spp_st_read_and_update(page, page_offset, &last_sig, &curr_sig, &delta);

  /* The demand also counts as the *use* of any in-flight prefetched line. */
  spp_filter_check(addr, SPP_FILTER_DEMAND);

  /* Stage 2: train PT with the observed (sig, delta) correlation. */
  if(last_sig) spp_pt_update_pattern(last_sig, delta);

  /* Stage 3: lookahead loop. */
  const uns Q = PREF_SPP_MAX_DEPTH;
  if(Q == 0) return;

  /* Per-iter scratch space for read_pattern's outputs (PT_WAY entries
   * possible). We reset head/tail every outer iteration so the queue never
   * grows -- this avoids subtle overflow regardless of MAX_DEPTH or the PT
   * fan-out at any one signature. */
  const uns QLEN = PREF_SPP_PT_WAY + 1;
  int* delta_q = (int*)alloca(sizeof(int) * QLEN);
  uns* conf_q  = (uns*)alloca(sizeof(uns) * QLEN);

  Addr base_addr      = addr;
  uns  lookahead_conf = 100;
  uns  depth          = 0;
  Flag do_lookahead   = FALSE;

  do {
    uns lookahead_way = PREF_SPP_PT_WAY;
    uns pf_q_head     = 0;
    uns pf_q_tail     = 0;
    for(uns i = 0; i < QLEN; i++) { delta_q[i] = 0; conf_q[i] = 0; }
    spp_pt_read_pattern(curr_sig, delta_q, conf_q, &lookahead_way,
                        &lookahead_conf, &pf_q_tail, depth);

    do_lookahead = FALSE;
    /* Issue cutoff: callers can set ISSUE_THRESHOLD > PF_THRESHOLD to mute
     * low-confidence LLC-grade prefetches without shortening the lookahead
     * chain. ISSUE_THRESHOLD = 0 means "use PF_THRESHOLD" (paper default). */
    uns issue_cutoff = PREF_SPP_ISSUE_THRESHOLD
                         ? PREF_SPP_ISSUE_THRESHOLD : PREF_SPP_PF_THRESHOLD;
    while(pf_q_head < pf_q_tail) {
      uns this_conf = conf_q[pf_q_head];
      int this_delta = delta_q[pf_q_head];
      pf_q_head++;
      if(this_conf < issue_cutoff) {
        /* Below the issue cutoff: still keep the chain alive so deeper hops
         * get a chance, but don't issue this prefetch. */
        do_lookahead = TRUE;
        continue;
      }

      Addr pf_addr = (base_addr & ~((Addr)(DCACHE_LINE_SIZE - 1))) +
                     ((Addr)((int64)this_delta << log2_line));

      /* Same OS page: try to enqueue the prefetch. */
      if((addr & ~((Addr)(VA_PAGE_SIZE_BYTES - 1))) ==
         (pf_addr & ~((Addr)(VA_PAGE_SIZE_BYTES - 1)))) {
        Flag fill_l2 = (this_conf >= PREF_SPP_FILL_THRESHOLD);
        Spp_Filter_Req req =
          fill_l2 ? SPP_FILTER_L2_PREFETCH : SPP_FILTER_LLC_PREFETCH;
        if(spp_filter_check(pf_addr, req)) {
          Addr pf_line_index = pf_addr >> log2_line;
          Flag ok = pref_addto_ul1req_queue(proc_id, pf_line_index,
                                            spp->hwp_info->id);
          if(ok) {
            if(fill_l2) {
              STAT_EVENT(proc_id, PREF_SPP_PF_ISSUED_L2);
              spp->pf_issued++;
              if(spp->pf_issued > spp->global_counter_max) {
                spp->pf_issued >>= 1;
                spp->pf_useful >>= 1;
              }
            } else {
              STAT_EVENT(proc_id, PREF_SPP_PF_ISSUED_LLC);
            }
          }
        } else {
          STAT_EVENT(proc_id, PREF_SPP_PF_FILTERED);
        }
      } else if(PREF_SPP_GHR_ON) {
        /* Page-crossing -- record the would-be prefetch in the GHR so the
         * destination page can pick it up via spp_st_read_and_update. */
        STAT_EVENT(proc_id, PREF_SPP_PF_PAGE_CROSS);
        uns pf_offset = (uns)((pf_addr >> log2_line) & offs_mask);
        spp_ghr_update(curr_sig, this_conf, pf_offset, this_delta);
      }
      do_lookahead = TRUE;
    }

    /* Drive the next hop. */
    if(lookahead_way < PREF_SPP_PT_WAY) {
      uns set = (uns)(spp_hash64(curr_sig) % PREF_SPP_PT_SET);
      int chosen_delta = spp->pt[set].ways[lookahead_way].delta;
      base_addr += (Addr)((int64)chosen_delta << log2_line);
      uns sig_delta = spp_encode_sig_delta(chosen_delta);
      curr_sig =
        ((curr_sig << PREF_SPP_SIG_SHIFT) ^ sig_delta) & spp->sig_mask;
      if(lookahead_conf >= PREF_SPP_PF_THRESHOLD) depth++;
    } else {
      do_lookahead = FALSE;
    }
  } while(do_lookahead && depth < Q && PREF_SPP_LOOKAHEAD_ON);

  INC_STAT_EVENT(proc_id, PREF_SPP_LOOKAHEAD_DEPTH_TOTAL, depth);
  STAT_EVENT(proc_id, PREF_SPP_LOOKAHEAD_DEPTH_0 + (depth >= 9 ? 9 : depth));
}

/* ============================================================ */
/* HWP framework callbacks                                        */
/* ============================================================ */

void pref_spp_init(HWP* hwp) {
  if(!PREF_SPP_ON) return;
  ASSERTM(0,
          PREF_REPORT_PREF_MATCH_AS_HIT || PREF_REPORT_PREF_MATCH_AS_MISS,
          "SPP must train on demands that match outstanding prefetches\n");

  spp           = (Pref_SPP*)calloc(1, sizeof(Pref_SPP));
  spp->hwp_info = hwp->hwp_info;
  hwp->hwp_info->enabled = TRUE;

  /* Derived constants. */
  spp->sig_mask           = (1u << PREF_SPP_SIG_BIT) - 1u;
  spp->sig_delta_sign_bit = 1u << (PREF_SPP_SIG_DELTA_BIT - 1u);
  spp->st_tag_mask        = (1u << PREF_SPP_ST_TAG_BIT) - 1u;
  spp->filter_set         = 1u << PREF_SPP_QUOTIENT_BIT;
  spp->global_counter_max = (1u << PREF_SPP_GLOBAL_CTR_BIT) - 1u;

  /* Signature Table. */
  spp->st = (Spp_ST_Entry**)calloc(PREF_SPP_ST_SET, sizeof(Spp_ST_Entry*));
  for(uns s = 0; s < PREF_SPP_ST_SET; s++) {
    spp->st[s] = (Spp_ST_Entry*)calloc(PREF_SPP_ST_WAY, sizeof(Spp_ST_Entry));
    for(uns w = 0; w < PREF_SPP_ST_WAY; w++) spp->st[s][w].lru = w;
  }

  /* Pattern Table. */
  spp->pt = (Spp_PT_Set*)calloc(PREF_SPP_PT_SET, sizeof(Spp_PT_Set));
  for(uns s = 0; s < PREF_SPP_PT_SET; s++) {
    spp->pt[s].ways = (Spp_PT_Way*)calloc(PREF_SPP_PT_WAY, sizeof(Spp_PT_Way));
  }

  /* Prefetch Filter. */
  spp->filter =
    (Spp_Filter_Entry*)calloc(spp->filter_set, sizeof(Spp_Filter_Entry));

  /* GHR. */
  spp->ghr =
    (Spp_GHR_Entry*)calloc(PREF_SPP_GHR_ENTRIES, sizeof(Spp_GHR_Entry));

  spp->pf_useful = spp->pf_issued = 0;
  spp->global_accuracy_pct        = 0;
}

void pref_spp_done(void) {
  if(!spp) return;
  for(uns s = 0; s < PREF_SPP_ST_SET; s++) free(spp->st[s]);
  free(spp->st);
  for(uns s = 0; s < PREF_SPP_PT_SET; s++) free(spp->pt[s].ways);
  free(spp->pt);
  free(spp->filter);
  free(spp->ghr);
  free(spp);
  spp = NULL;
}

void pref_spp_ul1_miss(uns8 proc_id, Addr lineAddr, Addr loadPC,
                       uns32 global_hist) {
  (void)loadPC;
  (void)global_hist;
  if(!PREF_SPP_ON || spp == NULL) return;
  spp_operate(proc_id, lineAddr);
}

void pref_spp_ul1_hit(uns8 proc_id, Addr lineAddr, Addr loadPC,
                      uns32 global_hist) {
  (void)loadPC;
  (void)global_hist;
  /* Demands that match an in-flight prefetch are routed here (via the
   * PREF_REPORT_PREF_MATCH_AS_HIT framework knob). Treat them just like a
   * miss for SPP training so the prefetcher sees the whole demand stream. */
  if(!PREF_SPP_ON || spp == NULL) return;
  if(!PREF_REPORT_PREF_MATCH_AS_HIT) return;
  spp_operate(proc_id, lineAddr);
}

void pref_spp_ul1_pref_hit(uns8 proc_id, Addr lineAddr, Addr loadPC,
                           uns32 global_hist) {
  (void)proc_id;
  (void)loadPC;
  (void)global_hist;
  if(!PREF_SPP_ON || spp == NULL) return;
  /* A previously-issued SPP prefetch was actually used by a demand. The
   * filter increments pf_useful and pf_useful/pf_issued feeds back into
   * the alpha (global_accuracy_pct) used in PT.read_pattern. */
  spp_filter_check(lineAddr & ~((Addr)(DCACHE_LINE_SIZE - 1)),
                   SPP_FILTER_DEMAND);
}
