#!/bin/bash
# Verify the new ISSUE_THRESHOLD knob: ISSUE > PF_THRESHOLD should mute the
# LLC-grade leak on hashtable WITHOUT shortening the lookahead chain on
# productive workloads like linkedlist. Configs:
#   default:        (pf=25, issue=0)        — paper baseline
#   tuned:          (pf=40, depth=8)        — our knee from §5.9
#   tuned+issue=80: (pf=40, issue=80, d=8)  — proposed fix from §5.11
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_issue"
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
    local sent=$(grep "PREF_UL1REQ_QUEUE_SENTREQ " "$rundir/pref.stat.0.out" 2>/dev/null | awk '{print $2}')
    echo "${bench}_${label}: IPC=${ipc:-?} sent=${sent:-?}"
}

# Test on 3 benchmarks that span SPP's design points:
#   linkedlist — SPP's best case (productive chain)
#   2dstencil — partial benefit case
#   hashtable — failure case (the one we want to fix)
for bench in linkedlist 2dstencil hashtable; do
    run_one "$bench" "tuned" "--pref_spp_on 1
--pref_spp_pf_threshold 40
--pref_spp_max_depth 8"
    run_one "$bench" "tuned_issue60" "--pref_spp_on 1
--pref_spp_pf_threshold 40
--pref_spp_issue_threshold 60
--pref_spp_max_depth 8"
    run_one "$bench" "tuned_issue80" "--pref_spp_on 1
--pref_spp_pf_threshold 40
--pref_spp_issue_threshold 80
--pref_spp_max_depth 8"
done
