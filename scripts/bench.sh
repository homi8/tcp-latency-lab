#!/usr/bin/env bash
# Full benchmark: TCP always, gRPC when built. Writes per-run logs + CSVs and
# assembles results/summary.md from the raw client output (no parsing, no spin).
# Usage: bench.sh <bin_dir> <results_dir>
set -u
BIN="${1:?usage: bench.sh <bin_dir> <results_dir>}"
RES="${2:?usage: bench.sh <bin_dir> <results_dir>}"
mkdir -p "$RES"

wait_port() {
  python3 - "$1" "$2" <<'EOF'
import socket, sys, time
for _ in range(100):
    try:
        socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=1).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(1)
EOF
}

TCP_PORT=5000
GRPC_PORT=50051

start_tcp() { # tick-rate
  "$BIN/mock_exchange" --port $TCP_PORT --tick-rate "$1" > "$RES/tcp_server.log" 2>&1 &
  echo $!
  wait_port 127.0.0.1 $TCP_PORT
}
start_grpc() { # tick-rate
  "$BIN/grpc_exchange_server" --port $GRPC_PORT --tick-rate "$1" > "$RES/grpc_server.log" 2>&1 &
  echo $!
  wait_port 127.0.0.1 $GRPC_PORT
}
stop() { kill "$1" 2>/dev/null; wait "$1" 2>/dev/null; }

echo "=== TCP order-entry RTT (50k orders) ==="
SRV=$(start_tcp 0)
"$BIN/hft_client" --port $TCP_PORT --mode order --requests 50000 --csv "$RES/tcp_order.csv" | tee "$RES/tcp_order.log"
stop $SRV

echo "=== TCP throughput (200k orders, window 64) ==="
SRV=$(start_tcp 0)
"$BIN/hft_client" --port $TCP_PORT --mode throughput --requests 200000 --window 64 --csv "$RES/tcp_throughput.csv" | tee "$RES/tcp_throughput.log"
stop $SRV

echo "=== TCP tick-to-trade (20k ticks) ==="
SRV=$(start_tcp 10000)
"$BIN/hft_client" --port $TCP_PORT --mode tick2trade --ticks 20000 --csv "$RES/tcp_tick2trade.csv" | tee "$RES/tcp_tick2trade.log"
stop $SRV

if [[ -x "$BIN/grpc_exchange_server" && -x "$BIN/grpc_hft_client" ]]; then
  echo "=== gRPC order-entry RTT (5k orders) ==="
  SRV=$(start_grpc 0)
  "$BIN/grpc_hft_client" --port $GRPC_PORT --mode order --requests 5000 --csv "$RES/grpc_order.csv" | tee "$RES/grpc_order.log"
  stop $SRV

  echo "=== gRPC throughput (20k orders, 8 threads) ==="
  SRV=$(start_grpc 0)
  "$BIN/grpc_hft_client" --port $GRPC_PORT --mode throughput --requests 20000 --threads 8 --csv "$RES/grpc_throughput.csv" | tee "$RES/grpc_throughput.log"
  stop $SRV

  echo "=== gRPC tick-to-trade (2k ticks) ==="
  SRV=$(start_grpc 2000)
  "$BIN/grpc_hft_client" --port $GRPC_PORT --mode tick2trade --ticks 2000 --csv "$RES/grpc_tick2trade.csv" | tee "$RES/grpc_tick2trade.log"
  stop $SRV
else
  echo "SKIP grpc (not built)"
fi

# ---- summary.md ----
{
  echo "# Benchmark results"
  echo
  echo "Generated: $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo "Host: $(uname -srm)"
  if command -v sysctl >/dev/null; then sysctl -n machdep.cpu.brand_string 2>/dev/null || true; fi
  if [[ -r /proc/cpuinfo ]]; then grep -m1 'model name' /proc/cpuinfo; fi
  echo
  echo "Loopback (client and exchange on the same host). Raw client output below; CSVs alongside."
  for f in tcp_order tcp_throughput tcp_tick2trade grpc_order grpc_throughput grpc_tick2trade; do
    if [[ -f "$RES/$f.log" ]]; then
      echo
      echo "## $f"
      echo '```'
      cat "$RES/$f.log"
      echo '```'
    fi
  done
} > "$RES/summary.md"
echo "Wrote $RES/summary.md"
