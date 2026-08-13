#include "PRD.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

// One epoch injects C one-touch keys, so an epoch is approximately one cache
// turnover. Stable keys repeat every 4C; sparse keys repeat every 30C.
int main(int argc, char** argv) {
  try {
    std::size_t cap = 100, epochs = 240, stable = 64, sparse = 16, noise = 100;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto val = [&] { if (++i >= argc) throw std::runtime_error("missing value"); return std::string(argv[i]); };
      if (a == "--capacity") cap = std::stoull(val());
      else if (a == "--epochs") epochs = std::stoull(val());
      else if (a == "--stable") stable = std::stoull(val());
      else if (a == "--sparse") sparse = std::stoull(val());
      else if (a == "--noise") noise = std::stoull(val());
      else throw std::runtime_error("unknown argument: " + a);
    }
    const std::size_t key_count = stable + sparse + noise * epochs;
    std::cout << "lease,requests,hits,hit_ratio,stable_requests,stable_hits,stable_hit_ratio,sparse_requests,sparse_hits,sparse_hit_ratio,proof_expiries\n";
    for (double lease : {2.0, 4.0, 8.0, 16.0, 32.0}) {
      PRDCore cache(key_count, cap, 20, .95, 1, 0, false, false, true, true,
                    0, lease, true, false, 8.0);
      std::uint64_t requests = 0, hits = 0, stable_requests = 0, stable_hits = 0, sparse_requests = 0, sparse_hits = 0;
      auto access = [&](std::uint32_t key, bool is_stable, bool is_sparse) {
        ++requests;
        if (is_stable) ++stable_requests;
        if (is_sparse) ++sparse_requests;
        if (cache.Access(key, 10)) {
          ++hits;
          if (is_stable) ++stable_hits;
          if (is_sparse) ++sparse_hits;
        } else {
          cache.Admit(key, 10);
        }
      };
      for (std::size_t epoch = 0; epoch < epochs; ++epoch) {
        if (epoch % 4 == 0) {
          for (std::size_t k = 0; k < stable; ++k) access(k, true, false);
          if (epoch == 0) for (std::size_t k = 0; k < stable; ++k) access(k, true, false);
        }
        if (epoch % 30 == 0) {
          for (std::size_t k = 0; k < sparse; ++k) access(std::uint32_t(stable + k), false, true);
          if (epoch == 0) for (std::size_t k = 0; k < sparse; ++k) access(std::uint32_t(stable + k), false, true);
        }
        const std::uint32_t fresh_base = std::uint32_t(stable + sparse + epoch * noise);
        for (std::size_t k = 0; k < noise; ++k) access(fresh_base + std::uint32_t(k), false, false);
        cache.FinishBlock();
      }
      std::cout << lease << ',' << requests << ',' << hits << ','
                << std::setprecision(10) << double(hits) / requests << ','
                << stable_requests << ',' << stable_hits << ',' << (stable_requests ? double(stable_hits)/stable_requests : 0.0) << ','
                << sparse_requests << ',' << sparse_hits << ',' << (sparse_requests ? double(sparse_hits)/sparse_requests : 0.0) << ','
                << cache.counters().proof_expiries << '\n';
    }
  } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
