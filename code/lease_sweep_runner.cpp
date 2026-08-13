#include "PRD.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr int kDepth = 20;
struct Request { std::uint64_t epoch; std::uint32_t key; std::uint8_t depth; };
struct Trace { std::vector<Request> requests; std::size_t keys = 0; };
int Hex(char c) { if(c >= '0' && c <= '9') return c-'0'; if(c >= 'a' && c <= 'f') return c-'a'+10; if(c >= 'A' && c <= 'F') return c-'A'+10; return -1; }
struct Key { std::array<std::uint8_t,kDepth> bytes{}; std::uint8_t depth=0; bool operator==(const Key& x) const { return depth==x.depth && bytes==x.bytes; } };
struct Hash { std::size_t operator()(const Key& x) const { std::size_t h=x.depth; for(int i=0;i<x.depth;++i) h=h*131+x.bytes[i]; return h; } };

Trace LoadRaw(const std::string& path, std::uint64_t batch) {
  std::ifstream in(path); if(!in) throw std::runtime_error("cannot open: "+path);
  Trace t; std::unordered_map<Key,std::uint32_t,Hash> ids; std::string line; std::getline(in,line);
  std::uint64_t tx=0, current=~0ull; std::unordered_set<std::uint32_t> seen;
  auto add=[&](std::uint64_t epoch,std::string_view address) {
    if(address=="\"\"") return; if(address.size()!=42) throw std::runtime_error("bad address"); Key key;
    for(int i=0;i<kDepth;++i) { int h=Hex(address[2+2*i]),l=Hex(address[3+2*i]); if(h<0||l<0) throw std::runtime_error("bad hex"); key.bytes[i]=std::uint8_t(16*h+l); }
    for(int d=kDepth;d>=1;--d) { key.depth=std::uint8_t(d); auto[it,_]=ids.emplace(key,std::uint32_t(ids.size())); if(seen.insert(it->second).second) t.requests.push_back({epoch,it->second,std::uint8_t(d)}); key.bytes[d-1]=0; }
  };
  while(std::getline(in,line)) { auto a=line.find(','),b=line.find(',',a+1); if(a==std::string::npos||b==std::string::npos) continue; const auto epoch=tx++/batch; if(epoch!=current) { current=epoch; seen.clear(); } add(epoch,std::string_view(line).substr(a+1,b-a-1)); add(epoch,std::string_view(line).substr(b+1)); }
  t.keys=ids.size(); return t;
}

struct Result { std::uint64_t req=0,hits=0; PRDCore::DebugCounters counters{}; std::vector<PRDCore::LeaseControlSample> log; };
Result Run(const Trace& t, std::size_t cap, double lease, bool adaptive) {
  PRDCore cache(t.keys,cap,kDepth,.95,1,0,false,false,true,true,0,lease,true,false,8.0);
  if(adaptive) cache.EnableAdaptiveLease(); Result r; std::uint64_t epoch=t.requests.empty()?0:t.requests[0].epoch;
  for(const auto& x:t.requests) { if(x.epoch!=epoch) { cache.FinishBlock(); epoch=x.epoch; } ++r.req; if(cache.Access(x.key,x.depth)) ++r.hits; else cache.Admit(x.key,x.depth); }
  if(!t.requests.empty()) cache.FinishBlock(); r.counters=cache.counters(); r.log=cache.lease_control_samples(); return r;
}
}

int main(int argc,char**argv) { try {
  std::string trace,log_path; double ratio=0,lease=8; std::uint64_t batch=1; bool adaptive=false;
  for(int i=1;i<argc;++i) { std::string a=argv[i]; auto val=[&]{if(++i>=argc)throw std::runtime_error("missing value");return std::string(argv[i]);}; if(a=="--trace")trace=val(); else if(a=="--capacity-ratio")ratio=std::stod(val()); else if(a=="--batch-size")batch=std::stoull(val()); else if(a=="--lease-factor")lease=std::stod(val()); else if(a=="--adaptive")adaptive=true; else if(a=="--control-log")log_path=val(); else throw std::runtime_error("unknown argument: "+a); }
  if(trace.empty()||ratio<=0||batch==0) throw std::runtime_error("trace, capacity ratio, and batch size required"); Trace t=LoadRaw(trace,batch); const std::size_t cap=std::max<std::size_t>(1,std::size_t(t.keys*ratio)); Result r=Run(t,cap,lease,adaptive);
  std::cout<<"mode,batch_size,lease_factor,capacity_ratio,capacity,requests,hits,misses,hit_ratio,active_hits,late_reuses,proof_expiries\n";
  std::cout<<(adaptive?"adaptive":"fixed")<<','<<batch<<','<<lease<<','<<ratio<<','<<cap<<','<<r.req<<','<<r.hits<<','<<(r.req-r.hits)<<','<<std::setprecision(12)<<double(r.hits)/r.req<<','<<r.counters.lease_active_hits<<','<<r.counters.lease_late_reuses<<','<<r.counters.proof_expiries<<'\n';
  if(!log_path.empty()) { std::ofstream out(log_path); if(!out) throw std::runtime_error("cannot write log"); out<<"admission,lease_before,lease_after,active_hits,late_reuses,expiries\n"; for(const auto& x:r.log) out<<x.admission<<','<<x.lease_before<<','<<x.lease_after<<','<<x.active_hits<<','<<x.late_reuses<<','<<x.expiries<<'\n'; }
  return 0;
 } catch(const std::exception&e) { std::cerr<<"error: "<<e.what()<<'\n'; return 1; } }
