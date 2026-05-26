#!/bin/bash
# Sweep PF_THRESHOLD (the confidence cutoff at which SPP issues a prefetch)
# on the linkedlist benchmark - the workload where SPP's contribution is
# clearest. Each value runs at 10M instruction limit.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_sweep"
INST_LIMIT=10000000
BENCH=linkedlist

mkdir -p "$OUT"

run_one() {
    local pf_thresh=$1
    local rundir="$OUT/pf${pf_thresh}"
    mkdir -p "$rundir"
    local params="$OUT/PARAMS.pf${pf_thresh}.in"
    cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
    sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
    sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
    echo "--pref_spp_on 1" >> "$params"
    echo "--pref_spp_pf_threshold $pf_thresh" >> "$params"
    (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
        --program "$ROOT/benchmarks/bench_${BENCH}" \
        --param "$params" \
        --pintool_args "-fast_forward_to_start_inst 1" \
        --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
        > run.log 2>&1) || echo "  WARN: rc != 0 for pf${pf_thresh}"
    local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
    echo "pf${pf_thresh}: IPC=${ipc:-?}"
}

for thresh in 10 25 40 60 80; do
    run_one $thresh
done
