/* Naive square matrix multiply C = A × B (no blocking).
 *
 * Access pattern (per inner-loop iteration of i,j,k):
 *   A[i*N + k]: row-major sequential as k advances
 *   B[k*N + j]: stride-N (one row's worth) as k advances — page-crossing
 *   C[i*N + j]: outer-loop invariant, hit
 *
 * SPP's pitch: the +1 delta on A's row scan is a 100%-confidence path.
 * B's +N stride is a constant non-unit stride that SPP's PT should learn
 * as a single delta entry. C is reused; the prefetch filter should keep it
 * from issuing redundant prefetches. */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define N 512                  /* 512×512 doubles → 2 MB per matrix */

static double A[N][N];
static double B[N][N];
static double C[N][N];

static __attribute__((noinline)) double kernel(void) {
  double check = 0.0;
  for(int i = 0; i < N; i++) {
    for(int j = 0; j < N; j++) {
      double s = 0.0;
      for(int k = 0; k < N; k++) {
        s += A[i][k] * B[k][j];
      }
      C[i][j] = s;
    }
  }
  for(int i = 0; i < N; i++) check += C[i][i];
  return check;
}

int main(void) {
  for(int i = 0; i < N; i++) {
    for(int j = 0; j < N; j++) {
      A[i][j] = (double)(i + j);
      B[i][j] = (double)(i - j);
    }
  }
  scarab_begin();
  double s = kernel();
  scarab_end();
  return (int)((int64_t)s & 0xff);
}
