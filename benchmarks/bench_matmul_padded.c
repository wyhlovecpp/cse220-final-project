/* Padded matmul: same logical N=512 but row pitch padded to LD=520 so that
 * B[k*LD + j]'s per-k stride is 520×8 = 4160 B (NOT page-aligned), exactly
 * the §5.14 software-level fix. Should remove the SPP regression observed
 * on the un-padded N=512 matmul. */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define N  512       /* same logical size as the failure case in §5.13 */
#define LD 520       /* leading dimension, 8 extra doubles per row */
                     /* row pitch = 520 * 8 = 4160 B (NOT page-aligned) */

static double A[N * LD];
static double B[N * LD];
static double C[N * LD];

static __attribute__((noinline)) double kernel(void) {
  double check = 0.0;
  for(int i = 0; i < N; i++) {
    for(int j = 0; j < N; j++) {
      double s = 0.0;
      for(int k = 0; k < N; k++) {
        s += A[i * LD + k] * B[k * LD + j];
      }
      C[i * LD + j] = s;
    }
  }
  for(int i = 0; i < N; i++) check += C[i * LD + i];
  return check;
}

int main(void) {
  for(int i = 0; i < N; i++) {
    for(int j = 0; j < N; j++) {
      A[i * LD + j] = (double)(i + j);
      B[i * LD + j] = (double)(i - j);
    }
  }
  scarab_begin();
  double s = kernel();
  scarab_end();
  return (int)((int64_t)s & 0xff);
}
