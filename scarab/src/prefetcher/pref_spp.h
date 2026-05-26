/* Copyright 2026 CSE220 Final Project Group
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction. Provided "AS IS" without warranty.
 */

/***************************************************************************************
 * File         : pref_spp.h
 * Description  : Signature Path Prefetcher (SPP)
 *
 * Reference    : J. Kim, S. H. Pugsley, P. V. Gratz, A. L. N. Reddy, C. Wilkerson,
 *                Z. Chishti, "Path Confidence Based Lookahead Prefetching," MICRO 2016.
 ***************************************************************************************/

#ifndef __PREF_SPP_H__
#define __PREF_SPP_H__

#include "pref_common.h"

/* Filter request types (paper Section III). */
typedef enum Spp_Filter_Req_enum {
  SPP_FILTER_L2_PREFETCH,
  SPP_FILTER_LLC_PREFETCH,
  SPP_FILTER_DEMAND,
  SPP_FILTER_EVICT,
} Spp_Filter_Req;

/* Signature Table -- per-page recent access trail compressed into a signature. */
typedef struct Spp_ST_Entry_struct {
  Flag valid;
  uns  tag;          /* partial physical page tag (PREF_SPP_ST_TAG_BIT bits) */
  uns  sig;          /* PREF_SPP_SIG_BIT signature */
  uns  last_offset;  /* page offset of the last demand seen for this page */
  uns  lru;          /* LRU position within the set */
} Spp_ST_Entry;

/* Pattern Table -- delta histogram per signature. */
typedef struct Spp_PT_Way_struct {
  int delta;        /* signed cache-line delta */
  uns c_delta;      /* per-delta saturating confidence counter */
} Spp_PT_Way;

typedef struct Spp_PT_Set_struct {
  Spp_PT_Way* ways;
  uns         c_sig; /* per-set total counter; used as normaliser */
} Spp_PT_Set;

/* Prefetch Filter -- quotient-filter-style direct-mapped table for de-duping
 * outstanding prefetches and tracking usefulness. */
typedef struct Spp_Filter_Entry_struct {
  Flag valid;          /* an L2 prefetch was issued for this line */
  Flag useful;         /* the prefetched (or in-flight) line was used by a demand */
  uns  remainder_tag;  /* PREF_SPP_REMAINDER_BIT tag */
} Spp_Filter_Entry;

/* Global History Register -- carries one outstanding prefetch across a page
 * boundary so SPP can re-prime the ST on the *next* page. */
typedef struct Spp_GHR_Entry_struct {
  Flag valid;
  uns  sig;
  uns  confidence;
  uns  offset;        /* expected first offset on the destination page */
  int  delta;         /* delta used at the page-crossing point */
} Spp_GHR_Entry;

/* Top-level SPP state (single instance, shared across cores for now). */
typedef struct Pref_SPP_struct {
  HWP_Info* hwp_info;

  /* Tables. */
  Spp_ST_Entry**    st;     /* [PREF_SPP_ST_SET][PREF_SPP_ST_WAY] */
  Spp_PT_Set*       pt;     /* [PREF_SPP_PT_SET] */
  Spp_Filter_Entry* filter; /* [1 << PREF_SPP_QUOTIENT_BIT] */
  Spp_GHR_Entry*    ghr;    /* [PREF_SPP_GHR_ENTRIES] */

  /* Global counters (paper Section III, alpha). */
  uns64 pf_useful;
  uns64 pf_issued;
  uns   global_accuracy_pct;  /* 0..100 */

  /* Derived constants cached for speed (set in init). */
  uns sig_mask;
  uns sig_delta_sign_bit;    /* 1 << (PREF_SPP_SIG_DELTA_BIT - 1) */
  uns st_tag_mask;
  uns filter_set;
  uns global_counter_max;
} Pref_SPP;

/* HWP framework callbacks. */
void pref_spp_init(HWP* hwp);
void pref_spp_done(void);
void pref_spp_ul1_miss(uns8 proc_id, Addr lineAddr, Addr loadPC,
                       uns32 global_hist);
void pref_spp_ul1_hit(uns8 proc_id, Addr lineAddr, Addr loadPC,
                      uns32 global_hist);
void pref_spp_ul1_pref_hit(uns8 proc_id, Addr lineAddr, Addr loadPC,
                           uns32 global_hist);

#endif /* __PREF_SPP_H__ */
