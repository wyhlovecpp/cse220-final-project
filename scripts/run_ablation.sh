#!/bin/bash
# Ablation study for SPP: full vs no-GHR vs no-lookahead.
# Runs on two representative benchmarks (linkedlist and 2dstencil).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_ablation"
INST_LIMIT=10000000

mkdir -p "$OUT"

run_one() {
    local bench=$1
    local label=$2
    local extra=$3
    local rundir="$OUT/${bench}_${label}"
    mkdir -p "$rundir"
    local params="$OUT/PARAMS.${bench}_${label}.in"
    # Use kaby_lake defaults; turn on framework + SPP; add the variant flags.
    cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
    sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
    sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
    echo "--pref_spp_on 1" >> "$params"
    echo "$extra" >> "$params"
    (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
        --program "$ROOT/benchmarks/bench_${bench}" \
        --param "$params" \
        --pintool_args "-fast_forward_to_start_inst 1" \
        --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
        > run.log 2>&1) || echo "  WARN: rc != 0 for $bench/$label"
    local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
    echo "$bench/$label: IPC=${ipc:-?}"
}

# Run ablation matrix on two representative benchmarks.
for bench in linkedlist 2dstencil; do
    run_one "$bench" "spp_full"           ""
    run_one "$bench" "spp_no_ghr"         "--pref_spp_ghr_on 0"
    run_one "$bench" "spp_no_lookahead"   "--pref_spp_lookahead_on 0"
done
