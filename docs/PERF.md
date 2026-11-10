# Hot-path methodology: from numbers to nanoseconds

The benchmark tells you *how slow*; this doc tells you *where*. Everything
builds with `-g -fno-omit-frame-pointer`, so profilers work out of the box.

## 1. Measure honestly first

- **Warmup is on by default** (`--warmup`): cold instruction/data caches and
  lazy connection setup otherwise dominate short runs.
- **Pin threads** (`--pin C`): keeps client and server off each other's core
  and off the scheduler's migration path. On Linux go further:
  `taskset -c 2,3`, `isolcpus`/`nohz_full`, performance governor.
- **Quote tails**: p99/p999/max/stddev in every summary. HFT optimizes the
  worst case; a faster mean with a worse max is a regression.
- **Loopback is a floor**: same-host numbers exclude NIC, switch, and fiber.
  They compare *software* correctly, which is this repo's job.

## 2. Linux: perf + flame graphs

```sh
# Client + server pinned, server ticking (terminal 1)
taskset -c 2 ./build/mock_exchange --port 5000 --tick-rate 100000

# Record the client's tick-to-trade run (terminal 2)
perf record -g -F 999 -- taskset -c 3 \
  ./build/hft_client --port 5000 --mode tick2trade --ticks 200000
perf report -g             # annotated call tree, find the hot path

# Flame graph (needs Brendan Gregg's stackcollapse or inferno)
perf script | stackcollapse-perf.pl | flamegraph.pl > client.svg
# ..or: cargo install inferno; perf script | inferno-collapse-perf | inferno > client.svg
```

Profiling note: the tick rate above deliberately oversubscribes the client so
the loop stays saturated. For reportable tick-to-trade numbers do the
opposite — pace the server below 1/RTT (bench.sh uses 10k/s for TCP) so each
tick is fresh; otherwise you measure queue drain, not reaction.

What you should see for the TCP client: most cycles in `recv`/`send` (syscall
+ copy), then framing/parse. That is the correct shape — the kernel boundary
is the cost center. If `malloc`/`free` show up hot, something regressed: the
hot path here reuses buffers and should barely allocate.

For the gRPC pair, expect wide stacks through the HTTP/2 layer, HPACK, and
protobuf serialize/parse — the overhead inventory from `docs/TRADEOFFS.md`
made visible.

Useful counters beyond `-g`:

```sh
perf stat -e cycles,instructions,cache-misses,context-switches,page-faults \
  ./build/hft_client --port 5000 --mode order --requests 50000
```

Context switches > ~0 and major faults > 0 during a pinned loopback run are
smoking guns for jitter (see §4).

## 3. macOS: no perf, use these

```sh
# Time Profiler via CLI (writes a .trace, open in Instruments)
xcrun xctrace record --template 'Time Profiler' --launch -- \
  ./build/make/hft_client --port 5000 --mode tick2trade --ticks 50000

# Quick CLI-only sample of a running process
sample <pid> 5 -file /tmp/sample.txt   # 5s sample, human-readable stacks
```

`--spin` exists for this platform comparison: blocking `recv` sleeps in the
kernel and pays a wakeup on every message; spinning burns a core but skips
the sleep/wakeup. Measure both, quote both.

## 4. Determinism checklist (kills tails, not means)

- [ ] No allocation on the hot path (fixed buffers, reserve up front).
- [ ] No locks on the hot path (this exchange is single-threaded; the TCP
      client too — the gRPC throughput mode deliberately shows locking cost).
- [ ] Pinned threads, isolated cores, performance governor (Linux).
- [ ] Transparent huge pages off or understood (latency spikes on collapse).
- [ ] `TCP_NODELAY` set (check: without it, sub-MSS writes wait up to 200 ms
      for the delayed ACK — watch p99 explode, then turn it back on).
- [ ] One-way timestamps from a single clock domain; across hosts, PTP with
      hardware timestamps or don't claim one-way numbers at all.

## 5. Exercises (do one before the interview)

1. **Find the copy**: the exchange appends each tick frame per subscriber.
   Profile 50 subscribers and watch `memcpy` climb. Fix options: reference
   counting, `sendmsg` gathering, multicast.
2. **Move the tail**: run tick-to-trade pinned vs unpinned, `--spin` vs
   blocking. Plot p50/p99/max for the four cells.
3. **Halve an RPC**: profile `grpc/order`, then enable protobuf arenas and
   a larger channel arg set. Record what moved and what didn't — "tuning
   narrows, structure remains" is a defensible conclusion.
4. **Count the syscalls**: `strace -c` (Linux) / `dtruss -c` (macOS) one order
   round trip on each transport. The syscall-count ratio predicts the
   latency ratio surprisingly well.
