#include "TwoTimescaleCacheSparse.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
constexpr int kDepth = 20;
constexpr int kMaxNodesPerTx = 40;

struct PrefixKey {
  std::array<std::uint8_t, 20> prefix{};
  std::uint8_t depth = 0;
  bool operator==(const PrefixKey& o) const {
    return depth == o.depth && prefix == o.prefix;
  }
};
struct PrefixHash {
  std::size_t operator()(const PrefixKey& k) const {
    std::size_t h = k.depth;
    for (auto b : k.prefix) h = h * 131 + b;
    return h;
  }
};
struct Tx {
  std::array<std::uint32_t, kMaxNodesPerTx> nodes{};
  std::uint64_t block = 0;
  std::uint8_t paths = 0;
};
struct Trace {
  std::vector<Tx> txs;
  std::size_t key_count = 0;
};

int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
std::array<std::uint8_t, 20> ParseAddress(std::string_view a) {
  if (a.size() != 42) throw std::runtime_error("bad address");
  std::array<std::uint8_t, 20> out{};
  for (int i = 0; i < 20; ++i) {
    out[i] = static_cast<std::uint8_t>(Hex(a[2 + 2*i]) * 16 + Hex(a[3 + 2*i]));
  }
  return out;
}
std::array<std::uint32_t, kDepth> InternPath(
    std::string_view a,
    std::unordered_map<PrefixKey, std::uint32_t, PrefixHash>& ids) {
  const auto bytes = ParseAddress(a);
  std::array<std::uint32_t, kDepth> out{};
  for (int d = kDepth; d >= 1; --d) {
    PrefixKey k;
    k.prefix = bytes;
    k.depth = static_cast<std::uint8_t>(d);
    std::fill(k.prefix.begin() + d, k.prefix.end(), 0);
    auto [it, inserted] = ids.emplace(k, static_cast<std::uint32_t>(ids.size()));
    out[kDepth - d] = it->second;
  }
  return out;
}
Trace Load(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open trace");
  std::string line;
  std::getline(in, line);
  std::unordered_map<PrefixKey, std::uint32_t, PrefixHash> ids;
  Trace t;
  while (std::getline(in, line)) {
    const auto a = line.find(',');
    const auto b = line.find(',', a + 1);
    if (a == std::string::npos || b == std::string::npos) continue;
    Tx tx;
    tx.block = std::stoull(line.substr(0, a));
    auto from = InternPath(std::string_view(line).substr(a + 1, b - a - 1), ids);
    std::copy(from.begin(), from.end(), tx.nodes.begin());
    tx.paths = 1;
    std::string_view to(line.data() + b + 1, line.size() - b - 1);
    if (!to.empty() && to != "\"\"") {
      auto p = InternPath(to, ids);
      std::copy(p.begin(), p.end(), tx.nodes.begin() + kDepth);
      tx.paths = 2;
    }
    t.txs.push_back(tx);
  }
  t.key_count = ids.size();
  return t;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " TRACE.csv CAPACITY\n";
    return 2;
  }
  const Trace t = Load(argv[1]);
  const std::size_t C = std::stoull(argv[2]);
  TwoTimescaleCacheSparse cache(t.key_count, C, kDepth);

  std::uint64_t current_block = UINT64_MAX;
  for (const Tx& tx : t.txs) {
    if (current_block == UINT64_MAX) current_block = tx.block;
    if (tx.block != current_block) {
      cache.EndEpoch();
      current_block = tx.block;
    }
    for (int p = 0; p < tx.paths; ++p) {
      for (int i = 0; i < kDepth; ++i) {
        cache.Access(tx.nodes[p*kDepth + i], kDepth - i, p == 1);
      }
    }
    cache.EndMicroUnit();  // transaction clock
  }
  if (current_block != UINT64_MAX) cache.EndEpoch();

  const auto& s = cache.stats();
  std::cout << std::fixed << std::setprecision(6)
            << "requests=" << s.requests
            << " hits=" << s.hits
            << " hr=" << (double)s.hits / s.requests
            << " mhr=" << (double)s.macro_hits / s.macro_first_touches
            << " ihr=" << (double)s.intra_hits / s.intra_requests
            << " peak_micro=" << s.peak_micro
            << " peak_total=" << s.peak_total_resident
            << " carries=" << s.boundary_carries
            << " spills=" << s.macro_to_micro_spills << '\n';
}
