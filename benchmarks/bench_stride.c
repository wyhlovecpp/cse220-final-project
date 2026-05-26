/* Sequential stride benchmark (ROI = inner loop). */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define N (1 << 21)   /* 16 MB array */
#define ITERS 2

static uint64_t buf[N];

static __attribute__((noinline)) uint64_t kernel(uint64_t sum) {
  for(int iter = 0; iter < ITERS; iter++) {
    for(size_t i = 0; i < N; i++) {
      sum += buf[i];
    }
  }
  return sum;
}

int main(void) {
  for(size_t i = 0; i < N; i++) buf[i] = (uint64_t)i;
  scarab_begin();
  uint64_t s = kernel(0);
  scarab_end();
  return (int)(s & 0xff);
}
