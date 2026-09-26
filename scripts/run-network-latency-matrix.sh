#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

# Run the direct-X11 interaction-latency matrix without touching production
# listeners. The benchmark creates and removes a private veth/netns per case.
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BENCH=${BENCH:-$PROJECT_ROOT/tools/benchmark/xrdp_console_bench.py}
BUILD_DIR=${XRDP_CONSOLE_BUILD_DIR:-$PROJECT_ROOT/build-direct-console}
AUTH=${1:?usage: $0 /path/to/readable-Xauthority}
DURATION=${DURATION:-30}
JITTER=${JITTER:-0}
OUT_ROOT=${OUT_ROOT:-$PROJECT_ROOT/results/network-latency-$(date +%Y%m%d)}

mkdir -p "$OUT_ROOT"
sudo -v

case_id=0
for churn in 0 15 30; do
    for delay in 0 2.5 5; do
        case_id=$((case_id + 1))
        display=$((120 + case_id))
        port=$((4200 + case_id * 10))
        output="$OUT_ROOT/churn-${churn}fps-delay-${delay}ms.txt"
        mode=namespace
        case_jitter=$JITTER
        if [ "$delay" = 0 ]; then
            mode=localhost
            # The localhost control cannot carry a netem impairment. A
            # nonzero jitter value would make the benchmark reject this case
            # before it starts, even though its base delay is zero.
            case_jitter=0
        fi
        echo "=== churn=${churn}fps one_way_delay=${delay}ms output=${output} ==="
        PYTHONUNBUFFERED=1 python3 -B "$BENCH" \
            --auth "$AUTH" --backend direct-x11 \
            --direct-graphics-transport rfx --mode input-roundtrip \
            --xrdp "$BUILD_DIR/_deps/xrdp-install/sbin/xrdp" \
            --direct-module "$BUILD_DIR/src/libxrdp_console.so" \
            --input-churn-fps "$churn" --duration "$DURATION" \
            --client-display "$display" --base-port "$port" \
            --network-mode "$mode" --network-delay-ms "$delay" \
            --network-jitter-ms "$case_jitter" \
            2>&1 | tee "$output"
    done
done

echo "matrix results: $OUT_ROOT"
