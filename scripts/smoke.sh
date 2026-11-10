#!/usr/bin/env bash
# Smoke test: start each exchange, run small client checks, expect exit 0.
# Usage: smoke.sh <bin_dir>
set -u
BIN="${1:?usage: smoke.sh <bin_dir>}"
FAIL=0

wait_port() { # host port
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

check() { # name + command...
  local name="$1"; shift
  if "$@" > /tmp/smoke_"$name".log 2>&1; then
    echo "PASS $name"
  else
    echo "FAIL $name (see /tmp/smoke_$name.log)"
    FAIL=1
  fi
}

# ---- TCP ----
"$BIN/mock_exchange" --port 5000 --tick-rate 20000 > /tmp/smoke_tcp_server.log 2>&1 &
SRV=$!
if wait_port 127.0.0.1 5000; then
  check tcp_order "$BIN/hft_client" --port 5000 --mode order --requests 500 --warmup 50
  check tcp_throughput "$BIN/hft_client" --port 5000 --mode throughput --requests 2000 --window 32
  check tcp_tick2trade "$BIN/hft_client" --port 5000 --mode tick2trade --ticks 500
else
  echo "FAIL tcp_server did not listen"; FAIL=1
fi
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null

# ---- gRPC (only if built) ----
if [[ -x "$BIN/grpc_exchange_server" && -x "$BIN/grpc_hft_client" ]]; then
  "$BIN/grpc_exchange_server" --port 50051 --tick-rate 20000 > /tmp/smoke_grpc_server.log 2>&1 &
  GSRV=$!
  if wait_port 127.0.0.1 50051; then
    check grpc_order "$BIN/grpc_hft_client" --port 50051 --mode order --requests 200 --warmup 20
    check grpc_throughput "$BIN/grpc_hft_client" --port 50051 --mode throughput --requests 800 --threads 4
    check grpc_tick2trade "$BIN/grpc_hft_client" --port 50051 --mode tick2trade --ticks 200
  else
    echo "FAIL grpc_server did not listen"; FAIL=1
  fi
  kill $GSRV 2>/dev/null; wait $GSRV 2>/dev/null
else
  echo "SKIP grpc (not built)"
fi

exit $FAIL
