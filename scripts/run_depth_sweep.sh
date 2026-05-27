#!/bin/bash
# Sweep MAX_DEPTH (cap on SPP's lookahead chain) on linkedlist.
# Uses PF_THRESHOLD=40 (the knee we found earlier) so depth is the
# only variable.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_depth"
INST_LIMIT=10000000
BENCH=linkedlist

mkdir -p "$OUT"

run_one() {
    local depth=$1
    local rundir="$OUT/d${depth}"
    mkdir -p "$rundir"
    local params="$OUT/PARAMS.d${depth}.in"
    cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
    sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
    sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
    echo "--pref_spp_on 1" >> "$params"
    echo "--pref_spp_pf_threshold 40" >> "$params"
    echo "--pref_spp_max_depth $depth" >> "$params"
    (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
        --program "$ROOT/benchmarks/bench_${BENCH}" \
        --param "$params" \
        --pintool_args "-fast_forward_to_start_inst 1" \
        --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
        > run.log 2>&1) || echo "  WARN: rc != 0 for d${depth}"
    local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
    local avgdepth=""
    if [ -f "$rundir/pref.stat.0.out" ]; then
        local op=$(grep "PREF_SPP_OPERATE " "$rundir/pref.stat.0.out" | awk '{print $2}')
        local total=$(grep "PREF_SPP_LOOKAHEAD_DEPTH_TOTAL" "$rundir/pref.stat.0.out" | awk '{print $2}')
        avgdepth=$(awk "BEGIN{printf \"%.2f\", $total/$op}" 2>/dev/null)
    fi
    echo "d${depth}: IPC=${ipc:-?} avg_depth=${avgdepth:-?}"
}

for d in 1 2 4 8 16 32 64; do
    run_one $d
done
