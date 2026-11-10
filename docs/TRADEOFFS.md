# Raw TCP vs gRPC: where each belongs in trading

This repo benchmarks both transports on identical workloads so the tradeoff
is measured, not asserted. Run `scripts/bench.sh` and compare the
`tcp/*` and `grpc/*` sections of `results/summary.md`.

## What gRPC adds per message (the overhead inventory)

1. **HTTP/2 framing** — each RPC is DATA frames on a stream: length-prefix,
   stream IDs, flow-control windows, plus a background thread managing the
   connection (PINGs, window updates).
2. **Header compression (HPACK)** — `:method`, `:path`, `content-type`, etc.
   per call. Small after warmup, nonzero always.
3. **Protobuf encode/decode** — varint traversal, field dispatch, and
   allocation unless arenas are used. Our fixed 29-byte order becomes a
   heap-touching serialize + parse pair.
4. **Thread hops** — sync gRPC server: HTTP/2 reader → completion queue →
   handler thread. Each hop is a queue + wakeup. The TCP server here does
   accept → parse → ack inline on one core.
5. **Head-of-line coupling** — streams share one connection; a slow RPC can
   delay framing for others. TCP-per-session isolates more crudely but more
   predictably.

Expectation on loopback: TCP order RTT in the tens of microseconds, gRPC
several times that, with a much wider tail. The exact ratio is in your
`results/summary.md` — quote your own numbers, they differ per machine.

## Where each wins

| Path | Winner | Why |
|---|---|---|
| Market-data ingress | raw TCP / multicast | every microsecond of decode + dispatch is strategy delay |
| Order entry | raw TCP (binary) | same; venues speak binary (OUCH, FIX/FAST, ITCH) for this reason |
| Reference data, risk limits | gRPC | throughput-tolerant, schema evolution matters more |
| Back office, admin, config | gRPC | ergonomics, codegen, load balancing win |
| Inter-service (non-critical) | gRPC | observability (deadlines, tracing, retries) is free |

The interview-ready sentence: **gRPC optimizes developer velocity and
operability; the trading hot path optimizes the tail of a latency
distribution, so it pays for neither HTTP/2 nor protobuf there.**

## Fairness notes on this benchmark

- Both clients verify every response (ack IDs, tick sequence), so neither
  side skips work.
- gRPC runs with default channel/server options — production would tune
  (message caps, polling threads, arenas). Tuning narrows the gap; it does
  not close it, because items 1–4 above are structural
- Loopback flatters both and hides NIC/switch effects equally. It is the
  right place to compare *software* overhead, which is the point here.
- The TCP tick stream paces with `poll()` timeouts; the gRPC one sleeps per
  tick. Pacing coarseness affects inter-tick spacing, not the measured
  reaction latency.

## If you only remember one experiment

Run tick-to-trade on both and decompose it: `wire` vs `order-leg` vs total.
gRPC inflates the order leg (unary RPC per order) far more than the stream
leg — which is exactly why venues stream data over light protocols and keep
order entry binary too.
