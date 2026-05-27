#!/bin/bash
# Test the hypothesis (from §5.10) that hashtable's mild SPP regression is
# caused by the low-confidence LLC-grade prefetches. Sweep PF_THRESHOLD on
# the hashtable benchmark — at high enough threshold, the LLC-grade
# requests should disappear and the regression should vanish.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCARAB="$ROOT/scarab"
PIN_ROOT="$ROOT/tools/pin-3.15-98253-gb56e429b1-gcc-linux"
OUT="$ROOT/results_ht_pf"
INST_LIMIT=10000000

mkdir -p "$OUT"

run_one() {
    local pf=$1
    local rundir="$OUT/pf${pf}"
    mkdir -p "$rundir"
    local params="$OUT/PARAMS.pf${pf}.in"
    cat "$SCARAB/utils/qsort/PARAMS.qsort" > "$params"
    sed -i 's/^--pref_framework_on.*/--pref_framework_on 1/' "$params"
    sed -i 's/^--pref_stream_on.*/--pref_stream_on 0/' "$params"
    cat >> "$params" <<EOF
--pref_spp_on 1
--pref_spp_pf_threshold $pf
--pref_spp_max_depth 8
EOF
    (cd "$rundir" && PIN_ROOT="$PIN_ROOT" python3 "$SCARAB/bin/scarab_launch.py" \
        --program "$ROOT/benchmarks/bench_hashtable" \
        --param "$params" \
        --pintool_args "-fast_forward_to_start_inst 1" \
        --scarab_args "--inst_limit ${INST_LIMIT} --heartbeat_interval 2000000 --num_heartbeats 5" \
        > run.log 2>&1) || echo "  WARN: rc != 0 for pf${pf}"
    local ipc=$(grep -oP "IPC: \K[0-9.]+" "$rundir/core.stat.0.out" 2>/dev/null | head -1)
    local sent=$(grep "PREF_UL1REQ_QUEUE_SENTREQ " "$rundir/pref.stat.0.out" 2>/dev/null | awk '{print $2}')
    local l2=$(grep "PREF_SPP_PF_ISSUED_L2 " "$rundir/pref.stat.0.out" 2>/dev/null | awk '{print $2}')
    local llc=$(grep "PREF_SPP_PF_ISSUED_LLC " "$rundir/pref.stat.0.out" 2>/dev/null | awk '{print $2}')
    echo "pf${pf}: IPC=${ipc:-?} sent=${sent:-?} l2=${l2:-?} llc=${llc:-?}"
}

for pf in 25 40 60 80 90 95; do
    run_one $pf
done
