#include "LSBTconsecutive.hpp"
#include "LSBTdiscrete.hpp"
#include "LSBTSimple.hpp"
#include "PRD.hpp"
#include "PRDAblations.hpp"
#include "S3FifoBaseline.hpp"
#include "two_timescale/TwoTimescaleCacheSparse.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Entry { int version; std::uint32_t key; std::uint8_t depth; };
struct Trace { std::vector<Entry> entries; std::size_t keys = 0; };
struct Options { std::string algorithm = "PRD", trace; std::size_t capacity = 0;
  double ratio = 0.0, retention = 0.95; int max_depth = 20; bool self_test = false; };
struct Stats { std::uint64_t requests = 0, hits = 0, misses = 0, evictions = 0;
  std::uint64_t s3_main_evictions = 0, s3_main_eviction_reaccesses = 0,
      s3_main_eviction_requalifications = 0; double elapsed_ms = 0.0; };

// Classic LRU baseline (published textbook algorithm).
class LRUCache {
 public:
  struct Counters { std::uint64_t evictions = 0; };

  LRUCache(std::size_t, std::size_t capacity, int, double) : capacity_(capacity) {}

  bool Access(std::uint32_t key, int) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    order_.splice(order_.begin(), order_, it->second);
    return true;
  }

  bool Admit(std::uint32_t key, int) {
    if (capacity_ == 0) return false;
    auto it = map_.find(key);
    if (it != map_.end()) {
      order_.splice(order_.begin(), order_, it->second);
      return true;
    }
    if (order_.size() >= capacity_) {
      const std::uint32_t victim = order_.back();
      order_.pop_back();
      map_.erase(victim);
      ++counters_.evictions;
    }
    order_.push_front(key);
    map_[key] = order_.begin();
    return true;
  }

  void FinishBlock() {}
  const Counters& counters() const { return counters_; }

 private:
  std::size_t capacity_ = 0;
  std::list<std::uint32_t> order_;
  std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> map_;
  Counters counters_;
};

// Two-timescale cache (partner engineering v3): Macro (cross-version
// persistence) and Micro (within-version reuse) retention domains share one
// strict capacity budget. Access performs admission internally, so the runner
// interface maps onto Access/EndMicroUnit/EndEpoch.
class TwoTimescaleAdapter {
 public:
  struct Counters { std::uint64_t evictions = 0; };

  TwoTimescaleAdapter(std::size_t key_count, std::size_t capacity,
                      int max_depth, double retention)
      : cache_(key_count, capacity, max_depth, retention) {}

  bool Access(std::uint32_t key, int depth) {
    const bool hit = cache_.Access(key, depth, true);
    cache_.EndMicroUnit();  // fine-grained clock: tick once per request
    return hit;
  }

  bool Admit(std::uint32_t, int) { return true; }  // admission inside Access

  void FinishBlock() { cache_.EndEpoch(); }

  const Counters& counters() const { return counters_; }
  const auto& stats() const { return cache_.stats(); }

 private:
  TwoTimescaleCacheSparse cache_;
  Counters counters_;
};

void Usage(const char* p) {
  std::cerr << "Usage: " << p << " --trace PATH (--cache-size N | --capacity-ratio R)\n"
            << "  [--algorithm PRD|LSBTSimple|TwoTimescale|LSBTdiscrete|LSBTconsecutive|S3FIFO|S3FIFOGhost8|LRU"
            << "|PRDAdaptiveGhost|PRDAdaptiveLease|PRDNullDepth|PRDPureDepth|PRDColdFifo"
            << "|S3FIFOAdaptiveGhost|S3FIFOAdaptiveLease|S3FIFOAdaptiveGhostLease|S3FIFOMainCycleAudit]\n";
}
Options Parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) { std::string a = argv[i];
    auto v = [&] { if (++i >= argc) throw std::runtime_error("missing value"); return std::string(argv[i]); };
    if (a == "--algorithm") o.algorithm = v(); else if (a == "--trace") o.trace = v();
    else if (a == "--cache-size") o.capacity = std::stoull(v()); else if (a == "--capacity-ratio") o.ratio = std::stod(v());
    else if (a == "--history-retention") o.retention = std::stod(v()); else if (a == "--max-depth") o.max_depth = std::stoi(v());
    else if (a == "--self-test") o.self_test = true; else if (a == "--layer-protection") (void)v();
    else if (a == "--help" || a == "-h") { Usage(argv[0]); std::exit(0); }
    else throw std::runtime_error("unknown argument: " + a);
  }
  return o;
}
Trace Load(const std::string& path) {
  std::ifstream in(path); if (!in) throw std::runtime_error("cannot open trace: " + path);
  Trace t; std::unordered_map<std::string, std::uint32_t> ids; std::string line; std::getline(in, line);
  while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto a = line.find(','), b = line.find(',', a + 1); if (a == std::string::npos || b == std::string::npos) continue;
    auto [it, inserted] = ids.emplace(line.substr(b + 1), static_cast<std::uint32_t>(ids.size()));
    t.entries.push_back({std::stoi(line.substr(0, a)), it->second, static_cast<std::uint8_t>(std::stoi(line.substr(a + 1, b - a - 1)))});
  } t.keys = ids.size(); return t;
}
template <typename Cache> Stats Replay(const Trace& t, Cache& cache) {
  Stats s; const auto start = std::chrono::steady_clock::now(); int version = t.entries.empty() ? 0 : t.entries[0].version;
  for (const auto& e : t.entries) { if (e.version != version) { cache.FinishBlock(); version = e.version; }
    ++s.requests; if (cache.Access(e.key, e.depth)) ++s.hits; else { ++s.misses; cache.Admit(e.key, e.depth); } }
  if (!t.entries.empty()) cache.FinishBlock();
  s.elapsed_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now()-start).count();
  s.evictions = cache.counters().evictions; return s;
}
Stats ReplayS3(const Trace& t, std::size_t c, double ghost, bool ag, bool al, bool audit) {
  S3FifoBaseline cache(c, .10, ghost, 2); if (ag) cache.EnableAdaptiveGhost(); if (al) cache.EnableAdaptiveLease(); if (audit) cache.EnableMainEvictionAudit();
  Stats s; const auto start = std::chrono::steady_clock::now(); int version = t.entries.empty() ? 0 : t.entries[0].version;
  for (const auto& e : t.entries) { if (e.version != version) { cache.FinishVersion(); version = e.version; }
    ++s.requests; if (cache.Get(e.key)) ++s.hits; else { ++s.misses; cache.PutOnMiss(e.key); s.evictions += cache.TakeEvictedKeys().size(); } }
  if (!t.entries.empty()) cache.FinishVersion();
  s.elapsed_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now()-start).count();
  const auto& x = cache.counters(); s.s3_main_evictions=x.main_evictions; s.s3_main_eviction_reaccesses=x.main_eviction_reaccesses; s.s3_main_eviction_requalifications=x.main_eviction_requalifications; return s;
}
void Print(const std::string& name, std::size_t c, const Stats& s) {
  std::cout << name << ',' << c << ',' << s.requests << ',' << s.hits << ',' << s.misses << ',' << std::setprecision(12)
            << (s.requests ? double(s.hits)/s.requests : 0.) << ',' << s.elapsed_ms << ',' << s.evictions << ',' << s.s3_main_evictions << ',' << s.s3_main_eviction_reaccesses << ',' << s.s3_main_eviction_requalifications << '\n';
}
}  // namespace
int main(int argc, char** argv) { try { const Options o=Parse(argc,argv); if(o.self_test){ PRD p(8,4); p.Access(1,2); p.Admit(1,2); if(!p.Access(1,2)) throw std::runtime_error("PRD self-test"); std::cout<<"self-tests: PASS\n"; return 0; }
  if(o.trace.empty() || ((o.capacity==0)==(o.ratio<=0))) throw std::runtime_error("trace and exactly one capacity are required");
  const Trace t=Load(o.trace);
  const std::size_t c=o.capacity?o.capacity:std::max<std::size_t>(1,std::llround(o.ratio*t.keys));
  std::cout<<"algorithm,capacity,requests,hits,misses,hit_ratio,elapsed_ms,evictions,s3_main_evictions,s3_main_eviction_reaccesses,s3_main_eviction_requalifications\n";
  if(o.algorithm=="PRD"){PRD x(t.keys,c,o.max_depth,o.retention);Print("PRD",c,Replay(t,x));}
  else if(o.algorithm=="PRDAdaptiveGhost"){PRDAdaptiveGhost x(t.keys,c,o.max_depth,o.retention);Print("PRDAdaptiveGhost",c,Replay(t,x));}
  else if(o.algorithm=="PRDAdaptiveLease"){PRDAdaptiveLease x(t.keys,c,o.max_depth,o.retention);Print("PRDAdaptiveLease",c,Replay(t,x));}
  else if(o.algorithm=="PRDNullDepth"){PRDNullDepth x(t.keys,c,o.max_depth,o.retention);Print("PRDNullDepth",c,Replay(t,x));}
  else if(o.algorithm=="PRDPureDepth"){PRDPureDepth x(t.keys,c,o.max_depth,o.retention);Print("PRDPureDepth",c,Replay(t,x));}
  else if(o.algorithm=="PRDColdFifo"){PRDColdFifo x(t.keys,c,o.max_depth,o.retention);Print("PRDColdFifo",c,Replay(t,x));}
  else if(o.algorithm=="LSBTdiscrete"){LSBTdiscrete x(t.keys,c,o.max_depth,o.retention);Print("LSBTdiscrete",c,Replay(t,x));}
  else if(o.algorithm=="LSBTconsecutive"){LSBTconsecutive x(t.keys,c,o.max_depth,o.retention);Print("LSBTconsecutive",c,Replay(t,x));}
  else if(o.algorithm=="S3FIFO") Print("S3FIFO",c,ReplayS3(t,c,.9,false,false,false));
  else if(o.algorithm=="S3FIFOGhost8") Print("S3FIFOGhost8",c,ReplayS3(t,c,8,false,false,false));
  else if(o.algorithm=="S3FIFOAdaptiveGhost") Print("S3FIFOAdaptiveGhost",c,ReplayS3(t,c,8,true,false,false));
  else if(o.algorithm=="S3FIFOAdaptiveLease") Print("S3FIFOAdaptiveLease",c,ReplayS3(t,c,8,false,true,false));
  else if(o.algorithm=="S3FIFOAdaptiveGhostLease") Print("S3FIFOAdaptiveGhostLease",c,ReplayS3(t,c,8,true,true,false));
  else if(o.algorithm=="S3FIFOMainCycleAudit") Print("S3FIFOMainCycleAudit",c,ReplayS3(t,c,8,false,false,true));
  else if(o.algorithm=="LSBTSimple"){LSBTSimple x(t.keys,c,o.max_depth,o.retention);Print("LSBTSimple",c,Replay(t,x));}
  else if(o.algorithm=="LRU"){LRUCache x(t.keys,c,o.max_depth,o.retention);Print("LRU",c,Replay(t,x));}
  else if(o.algorithm=="TwoTimescale"){TwoTimescaleAdapter x(t.keys,c,o.max_depth,o.retention);Print("TwoTimescale",c,Replay(t,x));}
  else throw std::runtime_error("unsupported algorithm");
  return 0;
 } catch(const std::exception& e){std::cerr<<"error: "<<e.what()<<'\n';Usage(argv[0]);return 1;} }
