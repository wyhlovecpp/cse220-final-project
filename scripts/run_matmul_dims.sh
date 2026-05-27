#!/bin/bash
# Test the §5.13 hypothesis: SPP fails on matmul because B's k-stride is
# EXACTLY one OS page (N×8B = 4096B for N=512). Run with three different
# N values to see how the regression depends on stride/page alignment:
#   N=512 (8B*512 = 4096B = 1 page exactly)    -- the failure case
#   N=448 (8B*448 = 3584B = 7/8 page)          -- not aligned
#   N=384 (8B*384 = 3072B = 3/4 page)          -- not aligned
#   N=256 (8B*256 = 2048B = 1/2 page)          -- 32 lines, halfway
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_matmul_n"
INST_LIMIT=10000000

mkdir -p "$OUT"

run_pair() {
    local N=$1
    # Build a per-N binary by patching the #define and rebuilding.
    cp "$ROOT/benchmarks/bench_matmul.c" "$OUT/bench_matmul_N${N}.c"
    sed -i "s/^#define N 512/#define N ${N}/" "$OUT/bench_matmul_N${N}.c"
    (cd "$OUT" && gcc -O2 -static -march=nehalem -mno-avx -mno-avx2 -mno-bmi \
        -mno-bmi2 -mno-fma -mno-sse4.2 -fno-asynchronous-unwind-tables \
        -fno-stack-protector -I"$ROOT/scarab/utils" \
        -o "bench_matmul_N${N}" "bench_matmul_N${N}.c")

    for label in nopref spp; do
        local rundir="$OUT/N${N}_${label}"
        mkdir -p "$rundir"
        local params="$OUT/PARAMS.N${N}_${label}.in"
        cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
        if [ "$label" = "nopref" ]; then
            sed -i 's/^--pref_framework_on.*/--pref_framework_on 0/' "$params"
            sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
        else
            sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
            sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
            echo "--pref_spp_on 1" >> "$params"
        fi
        (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
            --program "$OUT/bench_matmul_N${N}" \
            --param "$params" \
            --pintool_args "-fast_forward_to_start_inst 1" \
            --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
            > run.log 2>&1) || echo "  WARN: N=${N} ${label} failed"
        local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
        echo "N=${N} ${label}: IPC=${ipc:-?}"
    done
}

for N in 512 448 384 256; do
    run_pair $N
done
