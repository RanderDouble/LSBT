#include "TwoTimescaleCacheSparse.hpp"
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " twitter_raw.csv CAPACITY\n";
    return 2;
  }
  const std::size_t C = std::stoull(argv[2]);
  const std::size_t L = 2 * C;  // frozen Twitter engineering approximation

  // First pass only to obtain dense key_count expected by the candidate API.
  std::ifstream in(argv[1]);
  if (!in) throw std::runtime_error("cannot open trace");
  std::string line;
  std::getline(in, line);
  std::uint32_t max_key = 0;
  while (std::getline(in, line)) {
    const auto a = line.find(',');
    const auto b = line.find(',', a + 1);
    if (a == std::string::npos || b == std::string::npos) continue;
    max_key = std::max(max_key, static_cast<std::uint32_t>(std::stoul(line.substr(b + 1))));
  }
  in.close();

  TwoTimescaleCacheSparse cache(static_cast<std::size_t>(max_key) + 1, C, 8);
  in.open(argv[1]);
  std::getline(in, line);
  std::size_t pos = 0;
  std::uint32_t last_path = UINT32_MAX;
  while (std::getline(in, line)) {
    const auto a = line.find(',');
    const auto b = line.find(',', a + 1);
    if (a == std::string::npos || b == std::string::npos) continue;
    const std::uint32_t path = static_cast<std::uint32_t>(std::stoul(line.substr(0, a)));
    const int depth = std::stoi(line.substr(a + 1, b - a - 1));
    const std::uint32_t key = static_cast<std::uint32_t>(std::stoul(line.substr(b + 1)));

    if (last_path != UINT32_MAX && path != last_path) cache.EndMicroUnit();
    last_path = path;
    if (pos == L) {
      cache.EndEpoch();
      pos = 0;
    }
    cache.Access(key, depth, true);
    ++pos;
  }
  if (pos != 0) cache.EndEpoch();

  const auto& s = cache.stats();
  std::cout << std::fixed << std::setprecision(9)
            << "L=" << L
            << " hr=" << (double)s.hits/s.requests
            << " mhr=" << (double)s.macro_hits/s.macro_first_touches
            << " ihr=" << (double)s.intra_hits/s.intra_requests
            << " peak_micro=" << s.peak_micro
            << " peak_total=" << s.peak_total_resident << '\n';
}
