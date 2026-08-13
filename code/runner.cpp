#include "PRD.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

struct Entry { int version; std::uint32_t key; std::uint8_t depth; };
struct Trace { std::vector<Entry> entries; std::size_t unique_keys = 0; };

static Trace Load(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open trace: " + path);
  Trace t; std::unordered_map<std::string, std::uint32_t> ids; std::string line;
  std::getline(in, line);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto a = line.find(','), b = line.find(',', a + 1);
    if (a == std::string::npos || b == std::string::npos) continue;
    const int v = std::stoi(line.substr(0, a));
    const int d = std::stoi(line.substr(a + 1, b - a - 1));
    const std::string key = line.substr(b + 1);
    auto [it, inserted] = ids.emplace(key, static_cast<std::uint32_t>(ids.size()));
    t.entries.push_back({v, it->second, static_cast<std::uint8_t>(d)});
  }
  t.unique_keys = ids.size();
  return t;
}

static void Usage(const char* p) {
  std::cerr << "Usage: " << p << " --trace PATH (--cache-size N | --capacity-ratio R)\n"
            << "       [--max-depth N] [--history-retention R]\n";
}

int main(int argc, char** argv) {
  try {
    std::string path; std::size_t cap = 0; double ratio = 0; int max_depth = 20;
    double retention = 0.95;
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto val = [&]() { if (++i >= argc) throw std::runtime_error("missing argument"); return std::string(argv[i]); };
      if (a == "--trace") path = val();
      else if (a == "--cache-size") cap = std::stoull(val());
      else if (a == "--capacity-ratio") ratio = std::stod(val());
      else if (a == "--max-depth") max_depth = std::stoi(val());
      else if (a == "--history-retention") retention = std::stod(val());
      else if (a == "--help" || a == "-h") { Usage(argv[0]); return 0; }
      else throw std::runtime_error("unknown argument: " + a);
    }
    const Trace t = Load(path);
    if (cap == 0) cap = static_cast<std::size_t>(t.unique_keys * ratio);
    if (cap == 0) throw std::runtime_error("capacity must be positive");
    PRD cache(t.unique_keys, cap, max_depth, retention);
    std::uint64_t hits = 0, requests = 0, misses = 0;
    int current = t.entries.empty() ? 0 : t.entries.front().version;
    const auto start = std::chrono::steady_clock::now();
    for (const Entry& e : t.entries) {
      if (e.version != current) { cache.FinishBlock(); current = e.version; }
      ++requests;
      if (cache.Access(e.key, e.depth)) ++hits;
      else { ++misses; cache.Admit(e.key, e.depth); }
    }
    if (!t.entries.empty()) cache.FinishBlock();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "algorithm,capacity,unique_keys,requests,hits,misses,hit_ratio,elapsed_ms\n"
              << "PRD," << cap << ',' << t.unique_keys << ',' << requests << ',' << hits << ',' << misses << ','
              << std::setprecision(12) << (requests ? static_cast<double>(hits) / requests : 0.0) << ',' << ms << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; Usage(argv[0]); return 2; }
}
