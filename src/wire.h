#pragma once
// Binary wire protocol between mock exchange and client.
//
// Frame: u32 big-endian length N, then N bytes: [u8 type][body...].
// Bodies are fixed-size little-endian structs (same-host bench simplification;
// production feeds pick one byte order and document it — see README).
//
// Message flow:
//   client -> server: Subscribe{on}          enable/disable the tick stream
//   client -> server: NewOrder{...}          order entry
//   server -> client: Ack{client_id,...}     exactly one ack per order
//   server -> client: Tick{seq,send_ns,...}  market data, seq starts at 1
//
// The exchange stamps send_ns / exch_recv_ns from its own steady clock. On
// loopback (same host, same clock) the client can therefore measure one-way
// wire latency directly. Across hosts that needs synchronized clocks (PTP).
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

namespace wire {

enum Msg : uint8_t { kSubscribe = 1, kNewOrder = 2, kAck = 3, kTick = 4 };

inline constexpr std::size_t kMaxFrame = 1 << 20;

#pragma pack(push, 1)
struct SubscribeBody {
  uint8_t on;
};
struct NewOrderBody {
  char symbol[8];
  uint8_t side;  // 0 = bid, 1 = ask
  int64_t price;  // integer price (e.g. cents)
  int32_t qty;
  uint64_t client_id;
};
struct AckBody {
  uint64_t client_id;
  uint8_t status;  // 0 = accepted
  uint64_t exch_recv_ns;  // exchange steady-clock ns when the order arrived
};
struct TickBody {
  char symbol[8];
  int64_t price;
  int32_t qty;
  uint64_t seq;
  uint64_t send_ns;  // exchange steady-clock ns when the tick was framed
};
#pragma pack(pop)

static_assert(sizeof(NewOrderBody) == 29, "NewOrderBody packing changed");
static_assert(sizeof(AckBody) == 17, "AckBody packing changed");
static_assert(sizeof(TickBody) == 36, "TickBody packing changed");

inline uint64_t steady_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline void put_u32be(char* p, uint32_t v) {
  p[0] = char(v >> 24);
  p[1] = char(v >> 16);
  p[2] = char(v >> 8);
  p[3] = char(v);
}

inline uint32_t get_u32be(const char* p) {
  return (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) |
         (uint32_t(uint8_t(p[2])) << 8) | uint32_t(uint8_t(p[3]));
}

// Build a complete frame (header + type + body) with a single allocation.
template <typename Body>
inline std::string encode_frame(Msg type, const Body& body) {
  std::string f;
  f.resize(4 + 1 + sizeof(Body));
  put_u32be(&f[0], uint32_t(1 + sizeof(Body)));
  f[4] = char(type);
  std::memcpy(&f[5], &body, sizeof(Body));
  return f;
}

inline void fill_symbol(char out[8], const char* sym) {
  std::memset(out, 0, 8);
  std::strncpy(out, sym, 8);
}

}  // namespace wire
