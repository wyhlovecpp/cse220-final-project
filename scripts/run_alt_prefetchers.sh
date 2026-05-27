#!/bin/bash
# Fill in the head-to-head matrix: run ghb and markov on the 5 benchmarks
# where we don't yet have data (linkedlist was done in results_extras).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_alt"
INST_LIMIT=10000000

mkdir -p "$OUT"

run_one() {
    local bench=$1
    local label=$2
    local extra=$3
    local rundir="$OUT/${bench}_${label}"
    mkdir -p "$rundir"
    local params="$OUT/PARAMS.${bench}_${label}.in"
    cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
    sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
    sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
    echo "$extra" >> "$params"
    (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
        --program "$ROOT/benchmarks/bench_${bench}" \
        --param "$params" \
        --pintool_args "-fast_forward_to_start_inst 1" \
        --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
        > run.log 2>&1) || echo "  WARN: rc != 0 for ${bench}_${label}"
    local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
    echo "${bench}_${label}: IPC=${ipc:-?}"
}

# Linkedlist already covered in §5.8 (results_extras/). Do the other five.
for bench in stride strided 2dstencil random hashtable; do
    run_one "$bench" "ghb" "--pref_ghb_on 1"
    run_one "$bench" "markov" "--pref_markov_on 1"
done
