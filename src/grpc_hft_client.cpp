// grpc_hft_client.cpp — same three benchmark modes as hft_client.cpp, over
// gRPC: unary PlaceOrder for order-entry RTT and throughput (threaded),
// server-streaming StreamTicks driving tick-to-trade.
//
//   grpc_hft_client --port 50051 --mode order --requests 5000
#include <signal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "exchange.grpc.pb.h"
#include "latency.h"
#include "sys.h"
#include "wire.h"

namespace {

struct Args {
  const char* host = "127.0.0.1";
  int port = 50051;
  const char* mode = "order";
  uint64_t requests = 5000;
  uint64_t ticks = 2000;
  uint64_t threads = 8;
  uint64_t warmup = 500;
  const char* csv = "";
  const char* symbol = "AAPL";
  int pin = -1;
};

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    std::string v;
    auto eq = k.find('=');
    if (eq != std::string::npos) {
      v = k.substr(eq + 1);
      k = k.substr(0, eq);
    } else if (i + 1 < argc) {
      v = argv[++i];
    } else {
      return false;
    }
    if (k == "--host")
      a.host = strdup(v.c_str());
    else if (k == "--port")
      a.port = std::stoi(v);
    else if (k == "--mode")
      a.mode = strdup(v.c_str());
    else if (k == "--requests")
      a.requests = std::stoull(v);
    else if (k == "--ticks")
      a.ticks = std::stoull(v);
    else if (k == "--threads")
      a.threads = std::stoull(v);
    else if (k == "--warmup")
      a.warmup = std::stoull(v);
    else if (k == "--csv")
      a.csv = strdup(v.c_str());
    else if (k == "--symbol")
      a.symbol = strdup(v.c_str());
    else if (k == "--pin")
      a.pin = std::stoi(v);
    else
      return false;
  }
  return a.port > 0 && a.port < 65536 && a.threads > 0 &&
         (std::string(a.mode) == "order" || std::string(a.mode) == "tick2trade" ||
          std::string(a.mode) == "throughput");
}

latencylab::Order make_order(uint64_t id, const char* symbol) {
  latencylab::Order o;
  o.set_symbol(symbol);
  o.set_side(int32_t(id & 1));
  o.set_price(15000 + int64_t(id % 10) - 5);
  o.set_qty(100);
  o.set_client_id(id);
  return o;
}

// One unary order; verifies the ack. Stubs are thread-safe; contexts are not,
// so each call gets a fresh context.
bool place_order(latencylab::Exchange::Stub* stub, const latencylab::Order& o) {
  grpc::ClientContext ctx;
  latencylab::Ack ack;
  grpc::Status st = stub->PlaceOrder(&ctx, o, &ack);
  return st.ok() && ack.client_id() == o.client_id() && ack.status() == 0;
}

std::shared_ptr<grpc::Channel> dial(const Args& a) {
  grpc::ChannelArguments ch_args;
  // Throughput tuning worth knowing: a larger per-message cap and more
  // polling threads would go here for production; defaults keep the
  // comparison honest for small messages.
  return grpc::CreateCustomChannel(
      std::string(a.host) + ":" + std::to_string(a.port),
      grpc::InsecureChannelCredentials(), ch_args);
}

int run_order(const Args& a) {
  auto stub = latencylab::Exchange::NewStub(dial(a));
  for (uint64_t i = 0; i < a.warmup; ++i)
    if (!place_order(stub.get(), make_order(i + 1, a.symbol))) {
      std::fprintf(stderr, "warmup failed\n");
      return 1;
    }
  latencylab::LatencyStats st;
  st.reserve(static_cast<std::size_t>(a.requests));
  auto t0 = latencylab::Clock::now();
  for (uint64_t i = 0; i < a.requests; ++i) {
    uint64_t id = a.warmup + i + 1;
    auto t_send = latencylab::Clock::now();
    if (!place_order(stub.get(), make_order(id, a.symbol))) {
      std::fprintf(stderr, "order %llu failed\n", (unsigned long long)id);
      return 1;
    }
    st.add(latencylab::micros_since(t_send));
  }
  double el = std::chrono::duration<double>(latencylab::Clock::now() - t0).count();
  // Bytes here count protobuf payload only; HTTP/2 + HPACK framing on top is
  // part of what makes this path slower (see docs/TRADEOFFS.md).
  latencylab::Order probe = make_order(1, a.symbol);
  std::size_t bytes =
      a.requests * (probe.ByteSizeLong() + 16);  // ~ack size, same order
  st.summarize("grpc/order", el, bytes);
  if (a.csv[0] && !st.write_csv(a.csv)) {
    std::perror("csv");
    return 1;
  }
  return 0;
}

int run_throughput(const Args& a) {
  auto stub = latencylab::Exchange::NewStub(dial(a));
  std::vector<latencylab::LatencyStats> per(a.threads);
  std::vector<std::thread> workers;
  auto t0 = latencylab::Clock::now();
  for (uint64_t t = 0; t < a.threads; ++t) {
    workers.emplace_back([&, t] {
      uint64_t base = t * (a.requests / a.threads);
      uint64_t count = (t + 1 == a.threads) ? a.requests - base
                                            : a.requests / a.threads;
      per[t].reserve(static_cast<std::size_t>(count));
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t id = base + i + 1;
        auto t_send = latencylab::Clock::now();
        if (!place_order(stub.get(), make_order(id, a.symbol))) {
          std::fprintf(stderr, "thread %llu order %llu failed\n",
                       (unsigned long long)t, (unsigned long long)id);
          return;
        }
        per[t].add(latencylab::micros_since(t_send));
      }
    });
  }
  for (auto& th : workers) th.join();
  double el = std::chrono::duration<double>(latencylab::Clock::now() - t0).count();
  latencylab::LatencyStats st;
  for (auto& p : per) st.merge(p);
  if (st.size() != a.requests) return 1;
  latencylab::Order probe = make_order(1, a.symbol);
  st.summarize("grpc/throughput", el,
               a.requests * (probe.ByteSizeLong() + 16));
  if (a.csv[0] && !st.write_csv(a.csv)) {
    std::perror("csv");
    return 1;
  }
  return 0;
}

int run_tick2trade(const Args& a) {
  auto stub = latencylab::Exchange::NewStub(dial(a));
  grpc::ClientContext ctx;
  latencylab::Subscribe sub;
  sub.set_on(true);
  std::unique_ptr<grpc::ClientReader<latencylab::Tick>> reader(
      stub->StreamTicks(&ctx, sub));
  latencylab::LatencyStats t2t, ord, oneway;
  t2t.reserve(static_cast<std::size_t>(a.ticks));
  ord.reserve(static_cast<std::size_t>(a.ticks));
  oneway.reserve(static_cast<std::size_t>(a.ticks));
  latencylab::Tick tick;
  uint64_t expect_seq = 0, gaps = 0, n = 0;
  bool first_tick = true;
  auto t0 = latencylab::Clock::now();
  while (n < a.ticks && reader->Read(&tick)) {
    auto t_recv = latencylab::Clock::now();
    uint64_t recv_ns = wire::steady_ns();
    if (first_tick) {
      first_tick = false;
    } else if (tick.seq() != expect_seq) {
      ++gaps;
    }
    expect_seq = tick.seq() + 1;
    oneway.add(double(recv_ns - tick.send_ns()) / 1000.0);
    auto t_send = latencylab::Clock::now();
    if (!place_order(stub.get(), make_order(n + 1, a.symbol))) {
      std::fprintf(stderr, "order %llu failed\n", (unsigned long long)(n + 1));
      ctx.TryCancel();
      return 1;
    }
    auto t_ack = latencylab::Clock::now();
    t2t.add(std::chrono::duration<double, std::micro>(t_ack - t_recv).count());
    ord.add(std::chrono::duration<double, std::micro>(t_ack - t_send).count());
    ++n;
  }
  double el = std::chrono::duration<double>(latencylab::Clock::now() - t0).count();
  ctx.TryCancel();
  grpc::Status st_ok = reader->Finish();
  (void)st_ok;  // cancel races Finish; per-order verification already passed
  if (n != a.ticks) {
    std::fprintf(stderr, "stream ended early: %llu/%llu ticks\n",
                 (unsigned long long)n, (unsigned long long)a.ticks);
    return 1;
  }
  t2t.summarize("grpc/tick2trade", el, 0);
  ord.summarize("grpc/tick2trade/order-leg", el, 0);
  oneway.summarize("grpc/tick2trade/wire", el, 0);
  std::printf("[grpc/tick2trade] seq_gaps=%llu ticks_per_sec=%.0f\n",
              (unsigned long long)gaps, a.ticks / el);
  if (a.csv[0]) {
    FILE* f = std::fopen(a.csv, "w");
    if (!f) {
      std::perror("csv");
      return 1;
    }
    std::fprintf(f, "tick2trade_us,order_us,wire_us\n");
    for (std::size_t i = 0; i < t2t.size(); ++i)
      std::fprintf(f, "%.3f,%.3f,%.3f\n", t2t.samples()[i], ord.samples()[i],
                   oneway.samples()[i]);
    std::fclose(f);
  }
  return gaps == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr,
                 "usage: grpc_hft_client [--host H] [--port P] [--mode "
                 "order|tick2trade|throughput] [--requests N] [--ticks N] "
                 "[--threads T] [--warmup N] [--csv F] [--symbol S] [--pin C]\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  if (args.pin >= 0) {
    bool ok = latencylab::pin_current_thread_to_core(args.pin);
    std::printf("[grpc_hft_client] pin core %d: %s\n", args.pin,
                ok ? "ok" : "n/a");
  }
  std::string mode = args.mode;
  if (mode == "order") return run_order(args);
  if (mode == "throughput") return run_throughput(args);
  if (mode == "tick2trade") return run_tick2trade(args);
  return 2;
}
