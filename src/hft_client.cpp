// hft_client.cpp — benchmark client for the mock exchange.
//
// Modes:
//   order       sequential NewOrder -> Ack; measures order-entry RTT.
//   tick2trade  subscribe to ticks; on each tick immediately send an order;
//               measures tick_recv -> ack_recv (tick-to-trade), the order leg,
//               and one-way wire latency (valid: same host, same clock).
//   throughput  pipelined orders with --window in flight; measures max rate.
//
//   hft_client --port 5000 --mode order --requests 50000 --csv order.csv
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "latency.h"
#include "sys.h"
#include "wire.h"

namespace {

bool g_spin = false;  // spin on EAGAIN instead of blocking in the kernel

bool set_nonblocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

int connect_to(const char* host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (g_spin && !set_nonblocking(fd)) {
    close(fd);
    return -1;
  }
  return fd;
}

bool send_all(int fd, const char* p, std::size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, 0);
    if (w > 0) {
      p += w;
      n -= static_cast<std::size_t>(w);
    } else if (w < 0 && (errno == EINTR || (g_spin && (errno == EAGAIN ||
                                                      errno == EWOULDBLOCK)))) {
      continue;  // spin: trade a core for minimum wakeup latency
    } else {
      return false;
    }
  }
  return true;
}

bool recv_all(int fd, char* p, std::size_t n) {
  while (n > 0) {
    ssize_t r = recv(fd, p, n, 0);
    if (r > 0) {
      p += r;
      n -= static_cast<std::size_t>(r);
    } else if (r == 0) {
      return false;
    } else if (errno == EINTR ||
               (g_spin && (errno == EAGAIN || errno == EWOULDBLOCK))) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

// Read one frame; returns the type byte, body (without type) in `out`.
bool read_frame(int fd, uint8_t& type, std::string& out) {
  char hdr[4];
  if (!recv_all(fd, hdr, 4)) return false;
  uint32_t len = wire::get_u32be(hdr);
  if (len < 1 || len > wire::kMaxFrame) return false;
  out.resize(len);
  if (!recv_all(fd, &out[0], len)) return false;
  type = static_cast<uint8_t>(out[0]);
  out.erase(0, 1);
  return true;
}

wire::NewOrderBody make_order(uint64_t id, const char* symbol) {
  wire::NewOrderBody o;
  wire::fill_symbol(o.symbol, symbol);
  o.side = uint8_t(id & 1);
  o.price = 15000 + int64_t(id % 10) - 5;
  o.qty = 100;
  o.client_id = id;
  return o;
}

struct Args {
  const char* host = "127.0.0.1";
  int port = 5000;
  const char* mode = "order";
  uint64_t requests = 50000;
  uint64_t ticks = 20000;
  uint64_t window = 64;
  uint64_t warmup = 2000;
  const char* csv = "";
  const char* symbol = "AAPL";
  int pin = -1;
  bool spin = false;
};

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--spin") {
      a.spin = true;
      continue;
    }
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
    else if (k == "--window")
      a.window = std::stoull(v);
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
  return a.port > 0 && a.port < 65536 && a.window > 0 &&
         (std::string(a.mode) == "order" || std::string(a.mode) == "tick2trade" ||
          std::string(a.mode) == "throughput");
}

bool send_order(int fd, const wire::NewOrderBody& o) {
  std::string f = wire::encode_frame(wire::kNewOrder, o);
  return send_all(fd, f.data(), f.size());
}

// Expect an Ack for `want_id`. Returns false on any mismatch.
bool expect_ack(int fd, uint64_t want_id) {
  uint8_t type;
  std::string body;
  if (!read_frame(fd, type, body)) return false;
  if (type != wire::kAck || body.size() != sizeof(wire::AckBody)) return false;
  wire::AckBody a;
  std::memcpy(&a, body.data(), sizeof(a));
  return a.client_id == want_id && a.status == 0;
}

int run_order(const Args& a) {
  int fd = connect_to(a.host, a.port);
  if (fd < 0) {
    std::perror("connect");
    return 1;
  }
  for (uint64_t i = 0; i < a.warmup; ++i) {  // warmup: page in code, paths
    if (!send_order(fd, make_order(i + 1, a.symbol)) ||
        !expect_ack(fd, i + 1)) {
      std::fprintf(stderr, "warmup failed\n");
      close(fd);
      return 1;
    }
  }
  latencylab::LatencyStats st;
  st.reserve(static_cast<std::size_t>(a.requests));
  auto t0 = latencylab::Clock::now();
  for (uint64_t i = 0; i < a.requests; ++i) {
    uint64_t id = a.warmup + i + 1;
    auto t_send = latencylab::Clock::now();
    if (!send_order(fd, make_order(id, a.symbol)) || !expect_ack(fd, id)) {
      std::fprintf(stderr, "order %llu failed\n", (unsigned long long)id);
      close(fd);
      return 1;
    }
    st.add(latencylab::micros_since(t_send));
  }
  double el = std::chrono::duration<double>(latencylab::Clock::now() - t0).count();
  close(fd);
  constexpr std::size_t kOrderFrame = 4 + 1 + sizeof(wire::NewOrderBody);
  constexpr std::size_t kAckFrame = 4 + 1 + sizeof(wire::AckBody);
  st.summarize("tcp/order", el, a.requests * (kOrderFrame + kAckFrame));
  if (a.csv[0] && !st.write_csv(a.csv)) {
    std::perror("csv");
    return 1;
  }
  return 0;
}

int run_throughput(const Args& a) {
  int fd = connect_to(a.host, a.port);
  if (fd < 0) {
    std::perror("connect");
    return 1;
  }
  latencylab::LatencyStats st;
  st.reserve(static_cast<std::size_t>(a.requests));
  std::deque<std::pair<uint64_t, latencylab::Clock::time_point>> in_flight;
  auto t0 = latencylab::Clock::now();
  uint64_t sent = 0, acked = 0;
  while (sent < a.window && sent < a.requests) {
    ++sent;
    in_flight.emplace_back(sent, latencylab::Clock::now());
    if (!send_order(fd, make_order(sent, a.symbol))) {
      std::fprintf(stderr, "send failed\n");
      close(fd);
      return 1;
    }
  }
  while (acked < a.requests) {
    uint8_t type;
    std::string body;
    if (!read_frame(fd, type, body)) {
      std::fprintf(stderr, "read failed\n");
      close(fd);
      return 1;
    }
    auto t_ack = latencylab::Clock::now();
    if (type != wire::kAck || body.size() != sizeof(wire::AckBody) ||
        in_flight.empty()) {
      std::fprintf(stderr, "protocol mismatch\n");
      close(fd);
      return 1;
    }
    wire::AckBody ack;
    std::memcpy(&ack, body.data(), sizeof(ack));
    auto [id, t_send] = in_flight.front();
    in_flight.pop_front();
    if (ack.client_id != id) {  // TCP preserves order; anything else is a bug
      std::fprintf(stderr, "ack id %llu, expected %llu\n",
                   (unsigned long long)ack.client_id, (unsigned long long)id);
      close(fd);
      return 1;
    }
    st.add(std::chrono::duration<double, std::micro>(t_ack - t_send).count());
    ++acked;
    if (sent < a.requests) {
      ++sent;
      in_flight.emplace_back(sent, latencylab::Clock::now());
      if (!send_order(fd, make_order(sent, a.symbol))) {
        std::fprintf(stderr, "send failed\n");
        close(fd);
        return 1;
      }
    }
  }
  double el = std::chrono::duration<double>(latencylab::Clock::now() - t0).count();
  close(fd);
  constexpr std::size_t kOrderFrame = 4 + 1 + sizeof(wire::NewOrderBody);
  constexpr std::size_t kAckFrame = 4 + 1 + sizeof(wire::AckBody);
  st.summarize("tcp/throughput", el, a.requests * (kOrderFrame + kAckFrame));
  if (a.csv[0] && !st.write_csv(a.csv)) {
    std::perror("csv");
    return 1;
  }
  return 0;
}

int run_tick2trade(const Args& a) {
  int fd = connect_to(a.host, a.port);
  if (fd < 0) {
    std::perror("connect");
    return 1;
  }
  wire::SubscribeBody sub{1};
  std::string sf = wire::encode_frame(wire::kSubscribe, sub);
  if (!send_all(fd, sf.data(), sf.size())) {
    std::fprintf(stderr, "subscribe failed\n");
    close(fd);
    return 1;
  }
  latencylab::LatencyStats t2t, ord, oneway;
  t2t.reserve(static_cast<std::size_t>(a.ticks));
  ord.reserve(static_cast<std::size_t>(a.ticks));
  oneway.reserve(static_cast<std::size_t>(a.ticks));
  // Ticks stream continuously, so order acks interleave with fresh ticks on
  // the wire. Like a real feed handler, we demultiplex: stash ticks that
  // arrive while an order is in flight, react to them in arrival order.
  struct PendingTick {
    uint64_t seq;
    uint64_t send_ns;
    latencylab::Clock::time_point t_recv;
    uint64_t recv_ns;
  };
  std::deque<PendingTick> pending;
  auto read_one_tick = [&]() -> bool {
    uint8_t type;
    std::string body;
    if (!read_frame(fd, type, body)) return false;
    PendingTick pt;
    pt.t_recv = latencylab::Clock::now();
    pt.recv_ns = wire::steady_ns();
    if (type != wire::kTick || body.size() != sizeof(wire::TickBody))
      return false;
    wire::TickBody t;
    std::memcpy(&t, body.data(), sizeof(t));
    pt.seq = t.seq;
    pt.send_ns = t.send_ns;
    pending.push_back(pt);
    return true;
  };
  uint64_t expect_seq = 0, gaps = 0;
  bool first_tick = true;
  auto t0 = latencylab::Clock::now();
  for (uint64_t i = 0; i < a.ticks; ++i) {
    if (pending.empty() && !read_one_tick()) {
      std::fprintf(stderr, "tick read failed\n");
      close(fd);
      return 1;
    }
    PendingTick pt = pending.front();
    pending.pop_front();
    // Gap detection, like real feed handlers: baseline on the first tick
    // (feeds never stop; we join mid-stream), then demand contiguity.
    if (first_tick) {
      first_tick = false;
    } else if (pt.seq != expect_seq) {
      ++gaps;
    }
    expect_seq = pt.seq + 1;
    oneway.add(double(pt.recv_ns - pt.send_ns) / 1000.0);
    uint64_t id = i + 1;
    auto t_send = latencylab::Clock::now();
    if (!send_order(fd, make_order(id, a.symbol))) {
      std::fprintf(stderr, "order %llu send failed\n", (unsigned long long)id);
      close(fd);
      return 1;
    }
    // Read until our ack arrives, stashing interleaved ticks for later reacts.
    latencylab::Clock::time_point t_ack;
    for (;;) {
      uint8_t type;
      std::string body;
      if (!read_frame(fd, type, body)) {
        std::fprintf(stderr, "order %llu ack read failed\n",
                     (unsigned long long)id);
        close(fd);
        return 1;
      }
      if (type == wire::kTick) {
        if (body.size() != sizeof(wire::TickBody)) {
          std::fprintf(stderr, "bad tick size\n");
          close(fd);
          return 1;
        }
        wire::TickBody t;
        std::memcpy(&t, body.data(), sizeof(t));
        PendingTick stashed;
        stashed.seq = t.seq;
        stashed.send_ns = t.send_ns;
        stashed.t_recv = latencylab::Clock::now();
        stashed.recv_ns = wire::steady_ns();
        pending.push_back(stashed);
        continue;
      }
      if (type != wire::kAck || body.size() != sizeof(wire::AckBody)) {
        std::fprintf(stderr, "order %llu: unexpected frame\n",
                     (unsigned long long)id);
        close(fd);
        return 1;
      }
      wire::AckBody ack;
      std::memcpy(&ack, body.data(), sizeof(ack));
      if (ack.client_id != id || ack.status != 0) {
        std::fprintf(stderr, "order %llu: bad ack\n", (unsigned long long)id);
        close(fd);
        return 1;
      }
      t_ack = latencylab::Clock::now();
      break;
    }
    t2t.add(std::chrono::duration<double, std::micro>(t_ack - pt.t_recv).count());
    ord.add(std::chrono::duration<double, std::micro>(t_ack - t_send).count());
  }
  double el = std::chrono::duration<double>(latencylab::Clock::now() - t0).count();
  wire::SubscribeBody off{0};
  std::string uf = wire::encode_frame(wire::kSubscribe, off);
  send_all(fd, uf.data(), uf.size());
  close(fd);
  constexpr std::size_t kTickFrame = 4 + 1 + sizeof(wire::TickBody);
  constexpr std::size_t kOrderFrame = 4 + 1 + sizeof(wire::NewOrderBody);
  constexpr std::size_t kAckFrame = 4 + 1 + sizeof(wire::AckBody);
  std::size_t bytes = a.ticks * (kTickFrame + kOrderFrame + kAckFrame);
  t2t.summarize("tcp/tick2trade", el, bytes);
  ord.summarize("tcp/tick2trade/order-leg", el, 0);
  oneway.summarize("tcp/tick2trade/wire", el, 0);
  std::printf("[tcp/tick2trade] seq_gaps=%llu ticks_per_sec=%.0f\n",
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
                 "usage: hft_client [--host H] [--port P] [--mode "
                 "order|tick2trade|throughput] [--requests N] [--ticks N] "
                 "[--window W] [--warmup N] [--csv F] [--symbol S] [--pin C] "
                 "[--spin]\n");
    return 2;
  }
  g_spin = args.spin;
  signal(SIGPIPE, SIG_IGN);
  if (args.pin >= 0) {
    bool ok = latencylab::pin_current_thread_to_core(args.pin);
    std::printf("[hft_client] pin core %d: %s\n", args.pin, ok ? "ok" : "n/a");
  }
  std::string mode = args.mode;
  if (mode == "order") return run_order(args);
  if (mode == "throughput") return run_throughput(args);
  if (mode == "tick2trade") return run_tick2trade(args);
  return 2;
}
