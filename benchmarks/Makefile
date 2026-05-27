# Build the benchmarks for Scarab.
#
# IMPORTANT: -march=nehalem prevents the compiler from emitting BMI2 / AVX2
# instructions that the Scarab PIN frontend's uop decoder does not yet
# recognise (e.g. BZHI, TZCNT in long-mode). -static avoids linking against
# the system glibc that *itself* uses BMI2 in hot paths.

CC      = gcc
CFLAGS  = -O2 -static -march=nehalem \
          -mno-avx -mno-avx2 -mno-bmi -mno-bmi2 -mno-fma -mno-sse4.2 \
          -fno-asynchronous-unwind-tables -fno-stack-protector

BENCHES = bench_stride bench_strided bench_2dstencil bench_linkedlist bench_random bench_hashtable bench_matmul

all: $(BENCHES)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<
	@if objdump -d $@ | grep -qE 'bzhi|pext|pdep|tzcnt|vpcmp'; then \
	  echo "WARNING: $@ still contains BMI2/AVX2 instructions"; \
	fi

clean:
	rm -f $(BENCHES)

.PHONY: all clean
