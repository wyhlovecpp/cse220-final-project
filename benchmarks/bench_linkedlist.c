/* Deterministic linked-list traversal -- pointer chasing through a flat
 * pool. */

#include <stddef.h>
#include <stdint.h>
#include "../scarab/utils/scarab_markers.h"

#define N (1 << 18)
#define ITERS 4

typedef struct Node {
  struct Node* next;
  uint64_t     val;
  uint64_t     pad[2];
} Node;

static Node pool[N];

static __attribute__((noinline)) uint64_t kernel(void) {
  uint64_t sum = 0;
  for(int iter = 0; iter < ITERS; iter++) {
    Node* p = &pool[0];
    while(p) {
      sum += p->val;
      p = p->next;
    }
  }
  return sum;
}

int main(void) {
  for(size_t i = 0; i < N; i++) {
    size_t nxt = (i + 3) & (N - 1);
    pool[i].val  = i;
    pool[i].next = (nxt == 0) ? NULL : &pool[nxt];
  }
  scarab_begin();
  uint64_t s = kernel();
  scarab_end();
  return (int)(s & 0xff);
}
