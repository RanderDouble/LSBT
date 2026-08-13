#include "PRD.hpp"
#include "LSBTdiscrete.hpp"
#include "LSBTconsecutive.hpp"
#include "S3FifoBaseline.hpp"
#include <array>
#include <chrono>
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
struct Request { std::uint64_t epoch; std::uint32_t key; std::uint8_t depth; };
struct Trace { std::vector<Request> requests; std::size_t keys=0; };
int Hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
struct PathKey { std::array<std::uint8_t,20> b{}; std::uint8_t d=0; bool operator==(const PathKey&x)const{return d==x.d&&b==x.b;} };
struct PathHash { std::size_t operator()(const PathKey&x)const{std::size_t h=x.d;for(int i=0;i<x.d;++i)h=h*131+x.b[i];return h;} };

Trace LoadRaw(const std::string& path,std::uint64_t B){
 std::ifstream in(path);if(!in)throw std::runtime_error("cannot open: "+path); Trace t; std::unordered_map<PathKey,std::uint32_t,PathHash> ids; std::string s;std::getline(in,s);
 std::uint64_t tx=0,cur=~0ull; std::unordered_set<std::uint32_t> seen;
 auto add=[&](std::uint64_t e,std::string_view a){if(a=="\"\"")return;if(a.size()!=42)throw std::runtime_error("bad address");PathKey k;for(int i=0;i<20;++i){int h=Hex(a[2+i*2]),l=Hex(a[3+i*2]);if(h<0||l<0)throw std::runtime_error("bad hex");k.b[i]=std::uint8_t(h*16+l);}for(int d=20;d>=1;--d){k.d=std::uint8_t(d);auto[it,_]=ids.emplace(k,std::uint32_t(ids.size()));if(seen.insert(it->second).second)t.requests.push_back({e,it->second,std::uint8_t(d)});k.b[d-1]=0;}};
 while(std::getline(in,s)){auto a=s.find(','),b=s.find(',',a+1);if(a==std::string::npos||b==std::string::npos)continue;std::uint64_t e=tx++/std::max<std::uint64_t>(1,B);if(e!=cur){cur=e;seen.clear();}add(e,std::string_view(s).substr(a+1,b-a-1));add(e,std::string_view(s).substr(b+1));}
 t.keys=ids.size();return t;
}
Trace LoadBlock(const std::string& path){std::ifstream in(path);if(!in)throw std::runtime_error("cannot open: "+path);Trace t;std::unordered_map<std::string,std::uint32_t> ids;std::string s;std::getline(in,s);while(std::getline(in,s)){auto a=s.find(','),b=s.find(',',a+1);if(a==std::string::npos||b==std::string::npos)continue;auto [it,_]=ids.emplace(s.substr(b+1),std::uint32_t(ids.size()));t.requests.push_back({std::stoull(s.substr(0,a)),it->second,std::uint8_t(std::stoi(s.substr(a+1,b-a-1)))});}t.keys=ids.size();return t;}
template<class C> std::pair<std::uint64_t,std::uint64_t> Replay(const Trace&t,C&c){std::uint64_t h=0,m=0,cur=t.requests.empty()?0:t.requests[0].epoch;for(auto&r:t.requests){if(r.epoch!=cur){c.FinishBlock();cur=r.epoch;}if(c.Access(r.key,r.depth))++h;else{++m;c.Admit(r.key,r.depth);}}if(!t.requests.empty())c.FinishBlock();return {h,m};}
std::pair<std::uint64_t,std::uint64_t> ReplayS3(const Trace&t,S3FifoBaseline&c){std::uint64_t h=0,m=0,cur=t.requests.empty()?0:t.requests[0].epoch;for(auto&r:t.requests){if(r.epoch!=cur){c.FinishVersion();cur=r.epoch;}if(c.Get(r.key))++h;else{++m;c.PutOnMiss(r.key);c.TakeEvictedKeys();}}if(!t.requests.empty())c.FinishVersion();return {h,m};}
void Usage(const char*p){std::cerr<<"Usage: "<<p<<" --format raw|block --trace PATH --capacity-ratio R [--batch-size B]\n";}
}
int main(int argc,char**argv){try{std::string f,p;double ratio=0;std::uint64_t B=1;for(int i=1;i<argc;++i){std::string a=argv[i];auto v=[&]{if(++i>=argc)throw std::runtime_error("missing value");return std::string(argv[i]);};if(a=="--format")f=v();else if(a=="--trace")p=v();else if(a=="--capacity-ratio")ratio=std::stod(v());else if(a=="--batch-size")B=std::stoull(v());else if(a=="--help"){Usage(argv[0]);return 0;}else throw std::runtime_error("unknown argument");}if(p.empty()||ratio<=0)throw std::runtime_error("trace and ratio required");Trace t=f=="raw"?LoadRaw(p,B):LoadBlock(p);std::size_t cap=std::max<std::size_t>(1,std::size_t(t.keys*ratio));std::cout<<"algorithm,batch_size,capacity_ratio,unique_keys,capacity,requests,hits,misses,hit_ratio,main_scans,main_requeues,main_evictions,main_eviction_reaccesses,main_eviction_requalifications,reaccess_rate,requalification_rate,requeue_per_scan\n";auto out=[&](const char*n,std::pair<std::uint64_t,std::uint64_t>x){std::uint64_t req=x.first+x.second;std::cout<<n<<','<<B<<','<<ratio<<','<<t.keys<<','<<cap<<','<<req<<','<<x.first<<','<<x.second<<','<<std::setprecision(12)<<(req?double(x.first)/req:0)<<",0,0,0,0,0,0,0,0\n";};{PRD c(t.keys,cap,20,.95);out("PRD",Replay(t,c));}{LSBTdiscrete c(t.keys,cap,20,.95);out("LSBTdiscrete",Replay(t,c));}{LSBTconsecutive c(t.keys,cap,20,.95);out("LSBTconsecutive",Replay(t,c));}{S3FifoBaseline c(cap,.1,.9,2);c.EnableMainEvictionAudit();auto x=ReplayS3(t,c);const auto&z=c.counters();std::uint64_t req=x.first+x.second;std::cout<<"S3FIFO,"<<B<<','<<ratio<<','<<t.keys<<','<<cap<<','<<req<<','<<x.first<<','<<x.second<<','<<std::setprecision(12)<<(req?double(x.first)/req:0)<<','<<z.main_scans<<','<<z.main_requeues<<','<<z.main_evictions<<','<<z.main_eviction_reaccesses<<','<<z.main_eviction_requalifications<<','<<(z.main_evictions?double(z.main_eviction_reaccesses)/z.main_evictions:0.)<<','<<(z.main_eviction_reaccesses?double(z.main_eviction_requalifications)/z.main_eviction_reaccesses:0.)<<','<<(z.main_scans?double(z.main_requeues)/z.main_scans:0.)<<'\n';}return 0;}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<'\n';return 1;}}
