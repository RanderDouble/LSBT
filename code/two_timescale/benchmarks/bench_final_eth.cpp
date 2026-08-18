#include "TwoTimescaleCache.hpp"
#include "TwoTimescaleCachePacked.hpp"
#include "TwoTimescaleCacheSparse.hpp"
#include <chrono>
#include <numeric>

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



class SingleV2 {
 public:
  SingleV2(size_t n,size_t C,int d):c_(n,C,d,0.95,8.0,false){}
  bool Access(uint32_t k,int d,bool=true){bool h=c_.Access(k,d); if(!h)c_.Admit(k,d); if(h)++hits_; return h;}
  void EndMicroUnit(){c_.Tick();}
  void EndEpoch(){}
  struct S{uint64_t hits;}; S stats()const{return{hits_};}
 private: LSBTElasticCore c_; uint64_t hits_=0;
};
class SinglePacked {
 public:
  SinglePacked(size_t n,size_t C,int):m_(n),c_(m_,C){}
  bool Access(uint32_t k,int d,bool=true){bool h=c_.Access(k,d); if(!h){uint32_t v;c_.Admit(k,v);} if(h)++hits_; return h;}
  void EndMicroUnit(){c_.Tick();}
  void EndEpoch(){}
  struct S{uint64_t hits;}; S stats()const{return{hits_};}
 private: std::vector<packed_lsbt::Meta> m_; packed_lsbt::MacroLSBT c_; uint64_t hits_=0;
};

template<class Cache>
uint64_t ReplayX(const Trace& t, size_t C) {
  Cache cache(t.key_count,C,kDepth); uint64_t cur=UINT64_MAX;
  for(const Tx& tx:t.txs){
    if(cur==UINT64_MAX)cur=tx.block;
    if(tx.block!=cur){cache.EndEpoch();cur=tx.block;}
    for(int p=0;p<tx.paths;++p) for(int i=0;i<kDepth;++i) cache.Access(tx.nodes[p*kDepth+i],kDepth-i,p==1);
    cache.EndMicroUnit();
  }
  if(cur!=UINT64_MAX) cache.EndEpoch(); return cache.stats().hits;
}
template<class Cache> double OneX(const Trace&t,size_t C,uint64_t&out){auto a=std::chrono::steady_clock::now();out=ReplayX<Cache>(t,C);auto b=std::chrono::steady_clock::now();return std::chrono::duration<double,std::milli>(b-a).count();}
template<class Cache> double MedX(const Trace&t,size_t C,int reps,uint64_t&h){std::vector<double>v;v.reserve(reps);OneX<Cache>(t,C,h);for(int i=0;i<reps;i++)v.push_back(OneX<Cache>(t,C,h));std::sort(v.begin(),v.end());return v[v.size()/2];}
int main(int argc,char**argv){if(argc<3)return 2;Trace t=Load(argv[1]);size_t C=std::stoull(argv[2]);int reps=argc>3?std::stoi(argv[3]):11;uint64_t hv2=0,hsp=0,hs=0;OneX<TwoTimescaleCache>(t,C,hv2);OneX<TwoTimescaleCacheSparse>(t,C,hsp);std::vector<double>a,b;for(int i=0;i<reps;i++){if(i%2==0){a.push_back(OneX<TwoTimescaleCache>(t,C,hv2));b.push_back(OneX<TwoTimescaleCacheSparse>(t,C,hsp));}else{b.push_back(OneX<TwoTimescaleCacheSparse>(t,C,hsp));a.push_back(OneX<TwoTimescaleCache>(t,C,hv2));}}auto med=[](std::vector<double>v){std::sort(v.begin(),v.end());return v[v.size()/2];};double x=med(a),y=med(b);double single=MedX<SingleV2>(t,C,reps,hs);std::cout<<std::fixed<<std::setprecision(3)<<C<<","<<hv2<<","<<x<<","<<y<<","<<x/y<<","<<single<<","<<y/single<<"\n";}