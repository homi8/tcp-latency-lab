// grpc_exchange_server.cpp — the same mock exchange over gRPC: unary
// PlaceOrder (order entry) plus server-streaming StreamTicks (market data).
// Compare its tick-to-trade numbers against the raw TCP server to see what
// HTTP/2 framing, HPACK, protobuf encode, and extra thread hops cost.
#include <signal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "exchange.grpc.pb.h"
#include "wire.h"

namespace {

grpc::Server* g_server = nullptr;
void on_sigint(int) {
  if (g_server) g_server->Shutdown();
}

uint64_t g_orders = 0, g_ticks = 0;

class ExchangeImpl final : public latencylab::Exchange::Service {
 public:
  ExchangeImpl(uint64_t tick_rate, std::string symbol)
      : interval_ns_(tick_rate ? 1000000000ull / tick_rate : 0),
        symbol_(std::move(symbol)) {}

  grpc::Status PlaceOrder(grpc::ServerContext*, const latencylab::Order* o,
                          latencylab::Ack* a) override {
    a->set_client_id(o->client_id());
    a->set_status(0);
    a->set_exch_recv_ns(wire::steady_ns());
    ++g_orders;
    return grpc::Status::OK;
  }

  grpc::Status StreamTicks(grpc::ServerContext* ctx,
                           const latencylab::Subscribe*,
                           grpc::ServerWriter<latencylab::Tick>* w) override {
    uint64_t seq = 0;
    int64_t price = 15000;
    while (!ctx->IsCancelled()) {
      latencylab::Tick t;
      t.set_symbol(symbol_);
      price += int64_t(seq % 7) - 3;
      t.set_price(price);
      t.set_qty(100);
      t.set_seq(++seq);
      t.set_send_ns(wire::steady_ns());
      ++g_ticks;
      if (!w->Write(t)) break;
      if (interval_ns_)
        std::this_thread::sleep_for(std::chrono::nanoseconds(interval_ns_));
    }
    return grpc::Status::OK;
  }

 private:
  uint64_t interval_ns_;
  std::string symbol_;
};

struct Args {
  int port = 50051;
  uint64_t tick_rate = 20000;
  std::string symbol = "AAPL";
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
    if (k == "--port")
      a.port = std::stoi(v);
    else if (k == "--tick-rate")
      a.tick_rate = std::stoull(v);
    else if (k == "--symbol")
      a.symbol = v;
    else
      return false;
  }
  return a.port > 0 && a.port < 65536;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr,
                 "usage: grpc_exchange_server [--port P] [--tick-rate R] "
                 "[--symbol S]\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  ExchangeImpl service(args.tick_rate, args.symbol);
  grpc::ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:" + std::to_string(args.port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::fprintf(stderr, "server build failed\n");
    return 1;
  }
  g_server = server.get();
  std::printf("grpc_exchange_server listening on 0.0.0.0:%d tick_rate=%llu\n",
              args.port, (unsigned long long)args.tick_rate);
  std::fflush(stdout);
  server->Wait();
  std::printf("grpc_exchange_server exit orders=%llu ticks=%llu\n",
              (unsigned long long)g_orders, (unsigned long long)g_ticks);
  return 0;
}
