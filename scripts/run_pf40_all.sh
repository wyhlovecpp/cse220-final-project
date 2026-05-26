#!/bin/bash
# Re-run SPP at PF_THRESHOLD=40 (the linkedlist knee) on every benchmark,
# to see if the knee generalises or is workload-specific.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_pf40"
INST_LIMIT=10000000

mkdir -p "$OUT"

run_one() {
    local bench=$1
    local rundir="$OUT/${bench}"
    mkdir -p "$rundir"
    local params="$OUT/PARAMS.${bench}.in"
    cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
    sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
    sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
    echo "--pref_spp_on 1" >> "$params"
    echo "--pref_spp_pf_threshold 40" >> "$params"
    (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
        --program "$ROOT/benchmarks/bench_${bench}" \
        --param "$params" \
        --pintool_args "-fast_forward_to_start_inst 1" \
        --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
        > run.log 2>&1) || echo "  WARN: rc != 0 for $bench"
    local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
    echo "${bench}: IPC=${ipc:-?}"
}

for bench in stride strided 2dstencil linkedlist random; do
    run_one "$bench"
done
