// mock_exchange.cpp — single-threaded mock exchange on raw POSIX sockets.
//
// Speaks the wire protocol from wire.h: streams Tick frames to subscribed
// clients at --tick-rate and answers every NewOrder with an Ack. One poll()
// loop drives accepts, reads, writes, and tick pacing: no threads, no locks,
// no per-message allocation beyond amortized buffer growth.
//
//   mock_exchange --port 5000 --tick-rate 20000
//
// --tick-rate 0 disables the tick stream (order-entry benchmarks).
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wire.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

uint64_t g_conns = 0, g_orders = 0, g_ticks = 0, g_bytes_out = 0;

// Slow-consumer guard: a connection this far behind gets dropped instead of
// growing memory without bound. Real feeds solve this with conflation or
// multicast; dropping is the honest small-scale version.
constexpr std::size_t kMaxPending = 64 << 20;

bool set_nonblocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

struct Conn {
  int fd = -1;
  std::string in;  // received bytes not yet parsed
  std::size_t in_off = 0;  // parsed prefix, compacted lazily
  std::string out;  // bytes queued for write
  std::size_t out_sent = 0;
  bool subscribed = false;
  bool dead = false;
};

std::size_t pending_out(const Conn& c) { return c.out.size() - c.out_sent; }

void close_conn(Conn& c) {
  if (c.fd >= 0) close(c.fd);
  c.fd = -1;
  c.dead = true;
}

int make_listener(const char* host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(fd, 128) != 0 || !set_nonblocking(fd)) {
    close(fd);
    return -1;
  }
  return fd;
}

const char* g_symbol = "AAPL";

// One client message. Returns false to drop the connection.
bool handle_msg(Conn& c, const char* body, uint32_t len) {
  uint8_t type = static_cast<uint8_t>(body[0]);
  if (type == wire::kSubscribe) {
    if (len != 1 + sizeof(wire::SubscribeBody)) return false;
    wire::SubscribeBody s;
    std::memcpy(&s, body + 1, sizeof(s));
    c.subscribed = (s.on != 0);
    return true;
  }
  if (type == wire::kNewOrder) {
    if (len != 1 + sizeof(wire::NewOrderBody)) return false;
    wire::NewOrderBody o;
    std::memcpy(&o, body + 1, sizeof(o));
    wire::AckBody a;
    a.client_id = o.client_id;
    a.status = 0;
    a.exch_recv_ns = wire::steady_ns();
    c.out.append(wire::encode_frame(wire::kAck, a));
    ++g_orders;
    return pending_out(c) <= kMaxPending;
  }
  return false;  // unknown message type
}

// Drain the socket into the connection buffer and parse complete frames.
// Returns false to drop the connection.
bool do_read(Conn& c) {
  char buf[65536];
  for (;;) {
    ssize_t r = recv(c.fd, buf, sizeof(buf), 0);
    if (r > 0) {
      c.in.append(buf, static_cast<std::size_t>(r));
    } else if (r == 0) {
      return false;  // orderly shutdown
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else if (errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  for (;;) {
    std::size_t avail = c.in.size() - c.in_off;
    if (avail < 4) break;
    uint32_t len = wire::get_u32be(c.in.data() + c.in_off);
    if (len < 1 || len > wire::kMaxFrame) return false;
    if (avail < 4u + len) break;  // partial frame, wait for more bytes
    if (!handle_msg(c, c.in.data() + c.in_off + 4, len)) return false;
    c.in_off += 4 + len;
  }
  if (c.in_off == c.in.size()) {
    c.in.clear();
    c.in_off = 0;
  } else if (c.in_off > 65536) {
    c.in.erase(0, c.in_off);  // amortized compaction, keeps hot path O(1)-ish
    c.in_off = 0;
  }
  return true;
}

bool do_write(Conn& c) {
  while (pending_out(c) > 0) {
    ssize_t w =
        send(c.fd, c.out.data() + c.out_sent, pending_out(c), 0);
    if (w > 0) {
      c.out_sent += static_cast<std::size_t>(w);
      g_bytes_out += static_cast<uint64_t>(w);
    } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else if (w < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  if (pending_out(c) == 0) {
    c.out.clear();
    c.out_sent = 0;
  }
  return true;
}

void accept_all(int listen_fd, std::vector<Conn>& conns) {
  for (;;) {
    int fd = accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
      break;  // EMFILE etc: drop the pending connection, keep serving
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (!set_nonblocking(fd)) {
      close(fd);
      continue;
    }
    Conn c;
    c.fd = fd;
    conns.push_back(std::move(c));
    ++g_conns;
  }
}

struct Args {
  const char* host = "127.0.0.1";
  int port = 5000;
  uint64_t tick_rate = 20000;  // ticks/sec; 0 = no tick stream
  const char* symbol = "AAPL";
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
    else if (k == "--tick-rate")
      a.tick_rate = static_cast<uint64_t>(std::stoull(v));
    else if (k == "--symbol")
      a.symbol = strdup(v.c_str());
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
                 "usage: mock_exchange [--host H] [--port P] [--tick-rate R] "
                 "[--symbol S]\n");
    return 2;
  }
  g_symbol = args.symbol;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  int listen_fd = make_listener(args.host, args.port);
  if (listen_fd < 0) {
    std::perror("listen");
    return 1;
  }
  std::printf("mock_exchange listening on %s:%d tick_rate=%llu\n", args.host,
              args.port, (unsigned long long)args.tick_rate);
  std::fflush(stdout);

  std::vector<Conn> conns;
  uint64_t seq = 0;
  int64_t price = 15000;  // $150.00 random walk
  const uint64_t interval_ns =
      args.tick_rate ? 1000000000ull / args.tick_rate : 0;
  uint64_t next_tick_ns = wire::steady_ns();

  while (!g_stop) {
    bool pacing = interval_ns > 0;
    int timeout_ms = -1;
    if (pacing) {
      uint64_t now = wire::steady_ns();
      timeout_ms =
          (now >= next_tick_ns) ? 0 : int((next_tick_ns - now) / 1000000);
    }
    std::vector<pollfd> pfds;
    pfds.reserve(conns.size() + 1);
    pfds.push_back({listen_fd, POLLIN, 0});
    for (auto& c : conns) {
      short ev = POLLIN;
      if (pending_out(c) > 0) ev |= POLLOUT;
      pfds.push_back({c.fd, ev, 0});
    }
    int n = poll(pfds.data(), pfds.size(), timeout_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("poll");
      break;
    }
    if (pfds[0].revents & POLLIN) accept_all(listen_fd, conns);
    for (std::size_t i = 0; i < conns.size(); ++i) {
      Conn& c = conns[i];
      short rev = pfds[i + 1].revents;
      if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
        close_conn(c);
        continue;
      }
      if ((rev & POLLIN) && !do_read(c)) {
        close_conn(c);
        continue;
      }
      if ((rev & POLLOUT) && !do_write(c)) close_conn(c);
    }
    // Sweep dead connections (swap-remove; order is irrelevant).
    for (std::size_t i = 0; i < conns.size();) {
      if (conns[i].dead) {
        close(conns[i].fd);
        conns[i] = std::move(conns.back());
        conns.pop_back();
      } else {
        ++i;
      }
    }
    // Tick pacing: broadcast at most a bounded burst per wakeup so a slow
    // poll cycle can't spiral into an unbounded catch-up loop.
    if (pacing) {
      uint64_t now = wire::steady_ns();
      int burst = 0;
      while (now >= next_tick_ns && burst < 4096) {
        wire::TickBody t;
        wire::fill_symbol(t.symbol, g_symbol);
        price += int64_t(seq % 7) - 3;
        t.price = price;
        t.qty = 100;
        t.seq = ++seq;
        t.send_ns = wire::steady_ns();
        std::string frame = wire::encode_frame(wire::kTick, t);
        for (auto& c : conns) {
          if (c.subscribed && !c.dead) {
            c.out.append(frame);
            if (pending_out(c) > kMaxPending) close_conn(c);
          }
        }
        ++g_ticks;
        next_tick_ns += interval_ns;
        ++burst;
      }
      if (burst == 4096) next_tick_ns = now + interval_ns;
    }
  }

  for (auto& c : conns) close_conn(c);
  close(listen_fd);
  std::printf("mock_exchange exit conns=%llu orders=%llu ticks=%llu out_bytes=%llu\n",
              (unsigned long long)g_conns, (unsigned long long)g_orders,
              (unsigned long long)g_ticks, (unsigned long long)g_bytes_out);
  return 0;
}
