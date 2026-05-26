/* Random access -- worst-case sanity check for any prefetcher. */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define N (1 << 20)
#define ITERS 2

static uint64_t buf[N];
static uint64_t state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t xorshift64(void) {
  uint64_t x = state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  state = x;
  return x;
}

static __attribute__((noinline)) uint64_t kernel(void) {
  uint64_t sum = 0;
  for(int iter = 0; iter < ITERS; iter++) {
    for(int i = 0; i < N; i++) {
      sum += buf[xorshift64() & (N - 1)];
    }
  }
  return sum;
}

int main(void) {
  for(size_t i = 0; i < N; i++) buf[i] = (uint64_t)i;
  scarab_begin();
  uint64_t s = kernel();
  scarab_end();
  return (int)(s & 0xff);
}
