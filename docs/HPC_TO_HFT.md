# HPC → HFT skill map

Coming from HPC, most of the machinery transfers — what changes is the
objective function: **throughput → tail latency**, **FLOPS → determinism**.
This repo is the translation layer.

## What you already have (say it in interviews)

| HPC skill | HFT translation | Where it shows here |
|---|---|---|
| MPI point-to-point / collectives | TCP sessions, multicast fan-out | `mock_exchange.cpp` poll loop |
| Thread pinning, NUMA placement | core isolation, cache warmth | `--pin`, `src/sys.h` |
| Custom allocators, pools | no-malloc hot path | fixed buffers, `reserve()` up front |
| SIMD kernels | fast protocol parsing | fixed-width structs today; SIMD price parsing is extension #1 |
| Roofline / Amdahl analysis | latency budgets (tick → order leg) | `tick2trade` decomposition |
| Weak/strong scaling plots | throughput vs window-size curves | `--window` sweep in `throughput` mode |
| Batching for bandwidth | batching vs latency tradeoff | Nagle demo (`docs/PERF.md` §4) |
| GPU/FPGA offload thinking | FPGA tick-to-trade (same idea, harder) | talk about it; don't build it yet |

## What to unlearn (or re-weight)

- **Mean → tail.** HPC reports average time-to-solution; trading reports
  p99/max. A 2× faster average with 10× worse max loses money. Every summary
  in this repo prints the tail first-class for this reason.
- **Throughput → reaction.** HPC overlaps comm/comp to hide latency; HFT
  can't overlap a reaction to a tick that just arrived. Busy-spin (`--spin`)
  instead of blocking is the same tradeoff you know from progress threads,
  pushed to the limit.
- **Checkpoint/restart → sequence recovery.** Model: MPI job dies, restart
  from checkpoint. Market-data model: detect the sequence gap, request
  retransmit, keep trading. `tick2trade` counts gaps; a retransmit request
  path is a good extension.
- **Collective time → wire time.** You optimized allreduce; now optimize the
  NIC→strategy→NIC path. Kernel bypass (DPDK/Onload) is GPUDirect's cousin:
  skip the copy, skip the kernel.

## Jargon decoder (minimum viable)

- **Tick-to-trade**: market-data arrival → order at the venue. The headline
  metric; this repo's `tick2trade` mode.
- **LOB / NBBO**: limit order book; national best bid/offer. The book you'd
  build from the tick stream next.
- **Colo**: servers in the exchange building. Why loopback numbers here are
  a floor: production adds a (short, equal-for-all) wire.
- **Jitter**: variance of latency. `sd`/`max` in every summary.
- **Hot path**: instructions between tick-in and order-out. Profiled in
  `docs/PERF.md`.
- **Conflation**: merging rapid book updates when consumers lag — the grown-up
  version of this exchange's drop-on-64MB policy.
- **OUCH / ITCH / FIX**: real binary order-entry / feed / (text) protocols.
  This repo's `wire.h` is their didactic shadow.

## Suggested narrative

"I spent [X] in HPC on [pinning/allocators/SIMD/MPI]. I wanted to move
toward latency engineering, so I built a mock exchange: binary TCP feed,
order entry, tick-to-trade harness. I measured [your numbers], profiled the
hot path with perf, and compared against a gRPC implementation to quantify
the framework overhead. Next I'm adding [order book / multicast / io_uring]."

That is a credible 90-second answer to "why HFT, why you" — and every claim
in it has a file and a number behind it.
