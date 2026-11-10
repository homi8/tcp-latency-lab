#pragma once
// Header-only latency statistics: records per-operation samples (µs) and
// prints min/p50/p90/p99/p999/max/mean/stddev plus throughput. Shared by the
// TCP and gRPC benchmark clients so their numbers are directly comparable.
//
// HFT note: we report tail (p99/p999/max) and jitter (stddev), not just the
// mean. Trading systems are judged on worst-case determinism.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

namespace latencylab {

using Clock = std::chrono::steady_clock;

inline double micros_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

class LatencyStats {
 public:
  void reserve(std::size_t n) { samples_.reserve(n); }
  void add(double micros) { samples_.push_back(micros); }
  std::size_t size() const { return samples_.size(); }
  const std::vector<double>& samples() const { return samples_; }
  void merge(const LatencyStats& o) {
    samples_.insert(samples_.end(), o.samples_.begin(), o.samples_.end());
  }

  void summarize(const std::string& label, double elapsed_sec,
                 std::size_t bytes_moved) const {
    if (samples_.empty()) {
      std::printf("[%s] no samples\n", label.c_str());
      return;
    }
    std::vector<double> s = samples_;
    std::sort(s.begin(), s.end());
    auto pct = [&](double p) {
      double idx = p * (s.size() - 1);
      std::size_t lo = static_cast<std::size_t>(idx);
      std::size_t hi = std::min(lo + 1, s.size() - 1);
      double frac = idx - lo;
      return s[lo] * (1.0 - frac) + s[hi] * frac;
    };
    double sum = std::accumulate(s.begin(), s.end(), 0.0);
    double mean = sum / s.size();
    double sq = 0;
    for (double v : s) sq += (v - mean) * (v - mean);
    double sd = std::sqrt(sq / s.size());
    std::printf("[%s] n=%zu elapsed=%.3fs throughput=%.0f op/s bw=%.2f MB/s\n"
                "  us  min=%8.1f p50=%8.1f p90=%8.1f p99=%8.1f p999=%8.1f "
                "max=%8.1f mean=%8.1f sd=%8.1f\n",
                label.c_str(), s.size(), elapsed_sec, s.size() / elapsed_sec,
                bytes_moved / elapsed_sec / 1e6, s.front(), pct(0.50),
                pct(0.90), pct(0.99), pct(0.999), s.back(), mean, sd);
  }

  bool write_csv(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "rtt_us\n");
    for (double v : samples_) std::fprintf(f, "%.3f\n", v);
    std::fclose(f);
    return true;
  }

 private:
  std::vector<double> samples_;
};

}  // namespace latencylab
