#!/bin/bash
# Test the hypothesis that the 7-bit sign-magnitude delta encoding's ±63
# cache-line limit is why GHR doesn't help on the 2-D stencil. We expand
# the encoding to 8 bits (±127 cache-line range) and rerun.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_sigdelta"
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
    local boots=""
    if [ -f "$rundir/pref.stat.0.out" ]; then
        boots=$(grep "PREF_SPP_GHR_BOOT " "$rundir/pref.stat.0.out" | awk '{print $2}')
    fi
    echo "${bench}_${label}: IPC=${ipc:-?} ghr_boots=${boots:-?}"
}

# Stencil — main test for the hypothesis
run_one 2dstencil "bit7_pf40"   "--pref_spp_on 1 --pref_spp_pf_threshold 40 --pref_spp_sig_delta_bit 7"
run_one 2dstencil "bit8_pf40"   "--pref_spp_on 1 --pref_spp_pf_threshold 40 --pref_spp_sig_delta_bit 8"
run_one 2dstencil "bit10_pf40"  "--pref_spp_on 1 --pref_spp_pf_threshold 40 --pref_spp_sig_delta_bit 10"

# Linkedlist — regression check (its deltas are tiny, bit=8 should not change anything)
run_one linkedlist "bit8_pf40"  "--pref_spp_on 1 --pref_spp_pf_threshold 40 --pref_spp_sig_delta_bit 8"

# Random — sanity that confidence gate still keeps SPP from polluting
run_one random "bit8_pf40"      "--pref_spp_on 1 --pref_spp_pf_threshold 40 --pref_spp_sig_delta_bit 8"
