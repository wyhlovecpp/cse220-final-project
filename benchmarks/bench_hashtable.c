/* Hash-table probe benchmark — irregular but structured access.
 *
 * Build a closed-addressing hash table with linked-list buckets (just like
 * a typical Redis dict or glibc hsearch). Then probe the table N times with
 * pseudo-random keys. The access pattern is:
 *   1. bucket index (random within the table) — random scatter
 *   2. walk the linked-list chain in that bucket — short, locality-preserving
 *
 * SPP's pitch on this workload: confidence-gate suppresses prefetches when
 * the bucket scatter is unpredictable; the within-bucket chain has a
 * learnable +offset stride. */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define N_BUCKETS (1 << 17)         /* 128 K buckets */
#define N_ENTRIES (1 << 18)         /* 256 K entries -- avg chain length 2 */
#define N_PROBES  (1 << 19)         /* 512 K probes */

typedef struct Entry {
  uint64_t      key;
  uint64_t      val;
  struct Entry* next;
} Entry;

static Entry pool[N_ENTRIES];
static Entry* table[N_BUCKETS];

static uint64_t xorshift_state = 0x9E3779B97F4A7C15ULL;
static inline uint64_t xorshift(void) {
  uint64_t x = xorshift_state;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  xorshift_state = x;
  return x;
}

static __attribute__((noinline)) uint64_t kernel(void) {
  uint64_t sum = 0;
  for(int p = 0; p < N_PROBES; p++) {
    uint64_t  key  = xorshift();
    Entry*    e    = table[key & (N_BUCKETS - 1)];
    while(e) {
      if(e->key == key) {
        sum += e->val;
        break;
      }
      e = e->next;
    }
  }
  return sum;
}

int main(void) {
  /* Populate the pool and chain entries into buckets in a deterministic
   * pattern so the linked-list traversal has an intra-bucket stride. */
  for(size_t i = 0; i < N_ENTRIES; i++) {
    pool[i].key = i * 0x9E3779B1ULL;            /* spreads across buckets */
    pool[i].val = i;
    size_t b    = pool[i].key & (N_BUCKETS - 1);
    pool[i].next = table[b];
    table[b]     = &pool[i];
  }
  scarab_begin();
  uint64_t s = kernel();
  scarab_end();
  return (int)(s & 0xff);
}
