#include "VersionDedupLSBT.hpp"
#include "VersionDedupCacheSparse.hpp"
#include <unordered_set>
#include <chrono>

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


struct E{uint32_t key;uint8_t depth;bool boundary;};
std::vector<E> Build(const Trace&t){std::vector<E>o;o.reserve(500000);uint64_t cur=UINT64_MAX;std::unordered_set<uint32_t>s;for(auto&tx:t.txs){if(cur==UINT64_MAX)cur=tx.block;if(tx.block!=cur){if(!o.empty())o.back().boundary=true;s.clear();cur=tx.block;}for(int p=0;p<tx.paths;p++)for(int i=0;i<kDepth;i++){uint32_t k=tx.nodes[p*kDepth+i];if(s.insert(k).second)o.push_back({k,(uint8_t)(kDepth-i),false});}}if(!o.empty())o.back().boundary=true;return o;}
template<class C>uint64_t R(const std::vector<E>&e,size_t n,size_t cap){C c(n,cap,kDepth);uint64_t h=0;for(auto&q:e){h+=c.Access(q.key,q.depth);if(q.boundary)c.EndEpoch();}return h;}
template<class C>double M(const std::vector<E>&e,size_t n,size_t cap,int reps,uint64_t&h){std::vector<double>v;for(int i=-1;i<reps;i++){auto a=std::chrono::steady_clock::now();h=R<C>(e,n,cap);auto b=std::chrono::steady_clock::now();if(i>=0)v.push_back(std::chrono::duration<double,std::milli>(b-a).count());}std::sort(v.begin(),v.end());return v[v.size()/2];}
int main(int argc,char**argv){auto t=Load(argv[1]);auto e=Build(t);size_t C=std::stoull(argv[2]);int reps=argc>3?std::stoi(argv[3]):11;uint64_t a,b;double x=M<VersionDedupLSBT>(e,t.key_count,C,reps,a),y=M<VersionDedupCacheSparse>(e,t.key_count,C,reps,b);std::cout<<std::fixed<<std::setprecision(3)<<"C="<<C<<" events="<<e.size()<<" hits="<<a<<" v2_ms="<<x<<" sparse_ms="<<y<<" speedup="<<x/y<<"\n";}
