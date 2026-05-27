#!/bin/bash
# Two additional studies:
#  (a) GHR_ENTRIES sweep on 2dstencil at pf=40 (testing the hypothesis that
#      GHR was capped by its 8-entry size on sweeping workloads).
#  (b) Two more Scarab built-in prefetchers (ghb, 2dc) as additional baselines
#      on linkedlist.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_extras"
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

# (a) GHR_ENTRIES sweep on 2dstencil at pf=40
for n in 8 16 32 64 128; do
    run_one 2dstencil "ghr${n}" \
"--pref_spp_on 1
--pref_spp_pf_threshold 40
--pref_spp_ghr_entries $n"
done

# (b) Additional Scarab built-in prefetchers on linkedlist
run_one linkedlist "ghb" "--pref_ghb_on 1"
run_one linkedlist "2dc" "--pref_2dc_on 1"
run_one linkedlist "markov" "--pref_markov_on 1"
