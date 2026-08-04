#include "LSBTconsecutive.hpp"
#include "LSBTdiscrete.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct TraceEntry {
  int block_number = 0;
  std::uint32_t key_id = 0;
  std::uint8_t depth = 0;
};

struct EncodedTrace {
  std::vector<TraceEntry> entries;
  std::size_t unique_keys = 0;
};

struct Options {
  std::string algorithm = "both";
  std::string trace_path;
  std::size_t cache_size = 0;
  double capacity_ratio = 0.0;
  int max_depth = 20;
  double history_retention = 0.95;
  double gain_scale = 51.2;
  double ghost_size_ratio = 8.0;
  std::size_t bucket_count = 0;
  int rescue_limit = -1;
  int protect_scan_budget = 16;
  bool layer_protection = true;
  bool self_test = false;
};

struct BlockSpan {
  std::size_t begin = 0;
  std::size_t end = 0;
};

struct DepthGroup {
  std::uint8_t depth = 0;
  std::size_t begin = 0;
  std::size_t end = 0;
};

struct Stats {
  std::uint64_t blocks = 0;
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t admission_rejects = 0;
  std::uint64_t evictions = 0;
  std::uint64_t protected_installs = 0;
  std::uint64_t protected_candidates = 0;
  std::uint64_t protected_rejects = 0;
  std::uint64_t rescues = 0;
  std::uint64_t stale_heap_pops = 0;
  double elapsed_ms = 0.0;
};

void Usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0
      << " --trace PATH (--cache-size N | --capacity-ratio X)\n"
      << "  [--algorithm LSBTdiscrete|LSBTconsecutive|both] [--max-depth N]\n"
      << "  [--history-retention X]\n"
      << "  [--gain-scale X] [--ghost-size-ratio X]\n"
      << "  [--bucket-count N] [--rescue-limit N]\n"
      << "  [--protect-scan-budget N] [--layer-protection true|false]\n"
      << "  " << argv0 << " --self-test\n";
}

bool ParseBool(const std::string& value) {
  if (value == "true" || value == "1") {
    return true;
  }
  if (value == "false" || value == "0") {
    return false;
  }
  throw std::runtime_error("expected true or false, got: " + value);
}

Options ParseOptions(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* name) -> std::string {
      if (++i >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[i];
    };
    if (arg == "--algorithm") {
      opts.algorithm = value("--algorithm");
    } else if (arg == "--trace") {
      opts.trace_path = value("--trace");
    } else if (arg == "--cache-size") {
      opts.cache_size = static_cast<std::size_t>(
          std::stoull(value("--cache-size")));
    } else if (arg == "--capacity-ratio") {
      opts.capacity_ratio = std::stod(value("--capacity-ratio"));
    } else if (arg == "--max-depth") {
      opts.max_depth = std::stoi(value("--max-depth"));
    } else if (arg == "--history-retention") {
      opts.history_retention = std::stod(value("--history-retention"));
    } else if (arg == "--gain-scale") {
      opts.gain_scale = std::stod(value("--gain-scale"));
    } else if (arg == "--ghost-size-ratio") {
      opts.ghost_size_ratio = std::stod(value("--ghost-size-ratio"));
    } else if (arg == "--bucket-count") {
      opts.bucket_count =
          static_cast<std::size_t>(std::stoull(value("--bucket-count")));
    } else if (arg == "--rescue-limit") {
      opts.rescue_limit = std::stoi(value("--rescue-limit"));
    } else if (arg == "--protect-scan-budget") {
      opts.protect_scan_budget = std::stoi(value("--protect-scan-budget"));
    } else if (arg == "--layer-protection") {
      opts.layer_protection = ParseBool(value("--layer-protection"));
    } else if (arg == "--self-test") {
      opts.self_test = true;
    } else if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (!opts.self_test) {
    if (opts.trace_path.empty()) {
      throw std::runtime_error("--trace is required");
    }
    if ((opts.cache_size == 0) == (opts.capacity_ratio <= 0.0)) {
      throw std::runtime_error(
          "specify exactly one of --cache-size and --capacity-ratio");
    }
    if (opts.algorithm != "LSBTdiscrete" &&
        opts.algorithm != "LSBTconsecutive" &&
        opts.algorithm != "both") {
      throw std::runtime_error(
          "--algorithm must be LSBTdiscrete, LSBTconsecutive, or both");
    }
  }
  return opts;
}

EncodedTrace LoadTrace(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open trace: " + path);
  }

  EncodedTrace trace;
  std::unordered_map<std::string, std::uint32_t> key_ids;
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("empty trace: " + path);
  }

  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const std::size_t first = line.find(',');
    const std::size_t second =
        first == std::string::npos ? first : line.find(',', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
      throw std::runtime_error("malformed CSV line " +
                               std::to_string(line_number));
    }

    const int block_number = std::stoi(line.substr(0, first));
    const int depth = std::stoi(line.substr(first + 1, second - first - 1));
    if (depth < 0 || depth > 255) {
      throw std::runtime_error("depth out of range at line " +
                               std::to_string(line_number));
    }
    std::string key = line.substr(second + 1);
    auto [it, inserted] = key_ids.try_emplace(
        std::move(key), static_cast<std::uint32_t>(key_ids.size()));
    trace.entries.push_back(TraceEntry{
        block_number, it->second, static_cast<std::uint8_t>(depth)});
  }
  trace.unique_keys = key_ids.size();
  return trace;
}

std::vector<BlockSpan> BuildBlockSpans(const EncodedTrace& trace) {
  std::vector<BlockSpan> spans;
  if (trace.entries.empty()) {
    return spans;
  }
  std::size_t begin = 0;
  int block = trace.entries.front().block_number;
  for (std::size_t i = 1; i < trace.entries.size(); ++i) {
    if (trace.entries[i].block_number != block) {
      spans.push_back(BlockSpan{begin, i});
      begin = i;
      block = trace.entries[i].block_number;
    }
  }
  spans.push_back(BlockSpan{begin, trace.entries.size()});
  return spans;
}

std::vector<DepthGroup> BuildDepthGroups(const EncodedTrace& trace,
                                         const BlockSpan& span) {
  std::vector<DepthGroup> groups;
  if (span.begin >= span.end) {
    return groups;
  }
  std::size_t begin = span.begin;
  std::uint8_t depth = trace.entries[begin].depth;
  for (std::size_t i = span.begin + 1; i <= span.end; ++i) {
    if (i != span.end && trace.entries[i].depth == depth) {
      continue;
    }
    groups.push_back(DepthGroup{depth, begin, i});
    if (i != span.end) {
      begin = i;
      depth = trace.entries[i].depth;
    }
  }
  return groups;
}

template <typename Cache>
Stats Replay(const EncodedTrace& trace, Cache& cache,
             bool layer_protection) {
  Stats stats;
  const std::vector<BlockSpan> spans = BuildBlockSpans(trace);
  const auto start = std::chrono::steady_clock::now();

  for (const BlockSpan& span : spans) {
    const std::vector<DepthGroup> groups = BuildDepthGroups(trace, span);
    for (std::size_t group_index = 0; group_index < groups.size();
         ++group_index) {
      const DepthGroup& group = groups[group_index];
      if (layer_protection && group_index + 1 < groups.size() &&
          groups[group_index + 1].depth + 1 == group.depth) {
        cache.BeginProtectedKeyInstall();
        const DepthGroup& next = groups[group_index + 1];
        for (std::size_t j = next.begin; j < next.end; ++j) {
          cache.ProtectKeyIfResident(trace.entries[j].key_id);
        }
      } else {
        cache.ClearProtectedKeys();
      }

      for (std::size_t i = group.begin; i < group.end; ++i) {
        const TraceEntry& entry = trace.entries[i];
        ++stats.requests;
        if (cache.Access(entry.key_id, entry.depth)) {
          ++stats.hits;
        } else {
          ++stats.misses;
          if (!cache.Admit(entry.key_id, entry.depth)) {
            ++stats.admission_rejects;
          }
        }
      }
    }
    cache.FinishBlock();
    ++stats.blocks;
  }

  stats.elapsed_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  const auto& counters = cache.counters();
  stats.evictions = counters.evictions;
  stats.protected_installs = counters.protected_installs;
  stats.protected_candidates = counters.protected_candidates;
  stats.protected_rejects = counters.protected_rejects;
  return stats;
}

Stats ReplayLSBTdiscrete(const EncodedTrace& trace, const Options& opts,
                        std::size_t capacity) {
  LSBTdiscrete cache(
      trace.unique_keys, capacity, opts.max_depth, opts.history_retention,
      1, opts.bucket_count, opts.rescue_limit,
      opts.protect_scan_budget, true, 0.92, 0.98, opts.gain_scale,
      LSBTdiscrete::kDefaultGainScale, 0.0, 0.25,
      true, opts.ghost_size_ratio);
  Stats stats = Replay(trace, cache, opts.layer_protection);
  stats.rescues = cache.counters().rescues;
  return stats;
}

Stats ReplayLSBTconsecutive(const EncodedTrace& trace, const Options& opts,
                           std::size_t capacity) {
  LSBTconsecutive cache(
      trace.unique_keys, capacity, opts.max_depth, opts.history_retention,
      opts.protect_scan_budget, 0.92, 0.98,
      opts.gain_scale, opts.ghost_size_ratio);
  Stats stats = Replay(trace, cache, opts.layer_protection);
  stats.stale_heap_pops = cache.counters().stale_heap_pops;
  return stats;
}

void PrintHeader() {
  std::cout
      << "trace,algorithm,unique_keys,capacity,capacity_ratio,blocks,requests,"
         "hits,misses,hit_ratio,admission_rejects,evictions,rescues,"
         "protected_installs,protected_candidates,protected_rejects,"
         "stale_heap_pops,elapsed_ms,million_requests_per_second\n";
}

void PrintRow(const Options& opts, const std::string& algorithm,
              const EncodedTrace& trace, std::size_t capacity,
              const Stats& stats) {
  const double hit_ratio = stats.requests == 0
                               ? 0.0
                               : static_cast<double>(stats.hits) /
                                     static_cast<double>(stats.requests);
  const double capacity_ratio = trace.unique_keys == 0
                                    ? 0.0
                                    : static_cast<double>(capacity) /
                                          static_cast<double>(trace.unique_keys);
  const double throughput = stats.elapsed_ms == 0.0
                                ? 0.0
                                : static_cast<double>(stats.requests) /
                                      stats.elapsed_ms / 1000.0;
  std::cout << opts.trace_path << ',' << algorithm << ',' << trace.unique_keys
            << ',' << capacity << ',' << std::fixed << std::setprecision(8)
            << capacity_ratio << ',' << stats.blocks << ',' << stats.requests
            << ',' << stats.hits << ',' << stats.misses << ',' << hit_ratio
            << ',' << stats.admission_rejects << ',' << stats.evictions << ','
            << stats.rescues << ',' << stats.protected_installs << ','
            << stats.protected_candidates << ',' << stats.protected_rejects
            << ',' << stats.stale_heap_pops << ',' << std::setprecision(3)
            << stats.elapsed_ms << ',' << throughput << '\n';
}

void RunSelfTests() {
  constexpr double kEpsilon = 1e-9;
  auto require = [](bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(std::string("self-test failed: ") + message);
    }
  };
  {
    LSBTconsecutive cache(4, 2, 20, 0.95);
    require(!cache.Access(1, 1), "first access must miss");
    require(cache.Admit(1, 1), "first key must be admitted");
    require(std::abs(cache.DebugCurrentHeat(1) - 51.2) < kEpsilon,
            "initial exact gain");
    cache.FinishBlock();
    require(std::abs(cache.DebugCurrentHeat(1) - 48.64) < kEpsilon,
            "one-version exact decay");
    require(cache.Access(1, 1), "resident access must hit");
    require(std::abs(cache.DebugCurrentHeat(1) - 99.84) < kEpsilon,
            "decayed heat plus exact gain");
  }
  {
    LSBTconsecutive cache(4, 2, 20, 0.95);
    require(!cache.Access(1, 1) && cache.Admit(1, 1), "admit key 1");
    require(!cache.Access(2, 1) && cache.Admit(2, 1), "admit key 2");
    require(cache.Access(1, 1), "second access to key 1");
    require(!cache.Access(3, 1) && cache.Admit(3, 1), "admit key 3");
    require(cache.Contains(1) && !cache.Contains(2) && cache.Contains(3),
            "minimum exact heat must be evicted");
    require(cache.counters().stale_heap_pops > 0,
            "stale heap entry must be discarded");
  }
  {
    LSBTconsecutive cache(4, 2, 20, 0.95, 2);
    require(!cache.Access(1, 1) && cache.Admit(1, 1), "admit protected key");
    require(!cache.Access(2, 1) && cache.Admit(2, 1), "admit victim key");
    cache.BeginProtectedKeyInstall();
    cache.ProtectKeyIfResident(1);
    require(!cache.Access(3, 1) && cache.Admit(3, 1),
            "admit around one protected minimum");
    require(cache.Contains(1) && !cache.Contains(2),
            "protected minimum must be skipped");
  }
  {
    LSBTconsecutive cache(4, 2, 20, 0.95, 2);
    require(!cache.Access(1, 1) && cache.Admit(1, 1), "admit key 1");
    require(!cache.Access(2, 1) && cache.Admit(2, 1), "admit key 2");
    cache.BeginProtectedKeyInstall();
    cache.ProtectKeyIfResident(1);
    cache.ProtectKeyIfResident(2);
    require(!cache.Access(3, 1), "new key must miss");
    require(!cache.Admit(3, 1), "all-protected admission must reject");
    require(cache.Contains(1) && cache.Contains(2) && !cache.Contains(3),
            "rejection must preserve protected residents");
  }
  std::cout << "self-tests: PASS\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options opts = ParseOptions(argc, argv);
    if (opts.self_test) {
      RunSelfTests();
      return 0;
    }

    const EncodedTrace trace = LoadTrace(opts.trace_path);
    const std::size_t capacity =
        opts.cache_size > 0
            ? opts.cache_size
            : std::max<std::size_t>(
                  1, static_cast<std::size_t>(std::llround(
                         opts.capacity_ratio *
                         static_cast<double>(trace.unique_keys))));

    PrintHeader();
    if (opts.algorithm == "LSBTdiscrete" || opts.algorithm == "both") {
      PrintRow(opts, "LSBTdiscrete", trace, capacity,
               ReplayLSBTdiscrete(trace, opts, capacity));
    }
    if (opts.algorithm == "LSBTconsecutive" || opts.algorithm == "both") {
      PrintRow(opts, "LSBTconsecutive", trace, capacity,
               ReplayLSBTconsecutive(trace, opts, capacity));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    Usage(argv[0]);
    return 1;
  }
}
