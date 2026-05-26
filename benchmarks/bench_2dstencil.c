/* 2-D 5-point stencil. Multiple per-cell deltas SPP should learn. */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define W 512
#define H (1 << 11)   /* 8 MB total */
#define ITERS 1
#define ELEM uint64_t

static ELEM grid[W * H];

static __attribute__((noinline)) ELEM kernel(ELEM sum) {
  for(int iter = 0; iter < ITERS; iter++) {
    for(int i = 1; i < H - 1; i++) {
      for(int j = 1; j < W - 1; j++) {
        ELEM c = grid[i * W + j];
        ELEM n = grid[(i - 1) * W + j];
        ELEM s = grid[(i + 1) * W + j];
        ELEM e = grid[i * W + (j + 1)];
        ELEM w = grid[i * W + (j - 1)];
        sum += c + n + s + e + w;
      }
    }
  }
  return sum;
}

int main(void) {
  for(int i = 0; i < W * H; i++) grid[i] = (ELEM)i;
  scarab_begin();
  ELEM s = kernel(0);
  scarab_end();
  return (int)(s & 0xff);
}
