#include "LSBTconsecutive.hpp"
#include "LSBTdiscrete.hpp"
#include "PRD.hpp"
#include "PRDAblations.hpp"
#include "S3FifoBaseline.hpp"

#include <algorithm>
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
#include <vector>

namespace {
constexpr int kDepth=20, kNodes=40;
struct Key { std::array<std::uint8_t,kDepth> p{}; std::uint8_t d=0; bool operator==(const Key& x)const{return d==x.d&&p==x.p;} };
struct Hash { std::size_t operator()(const Key& k)const{std::size_t h=k.d;for(int i=0;i<k.d;++i)h=h*131+k.p[i];return h;} };
struct Tx { std::array<std::uint32_t,kNodes> n{}; std::uint8_t paths=0; };
struct Trace { std::vector<Tx> tx; std::size_t keys=0; };
struct Opt { std::string alg="prd",path;std::size_t cap=0;double ratio=0,retention=.95;bool self=false; };
struct Stats {std::uint64_t req=0,hit=0,miss=0,evict=0;double ms=0;};
int Hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
std::array<std::uint32_t,kDepth> Intern(std::string_view a,std::unordered_map<Key,std::uint32_t,Hash>& ids){if(a.size()!=42)throw std::runtime_error("invalid address");Key k;for(int i=0;i<kDepth;++i){int h=Hex(a[2+i*2]),l=Hex(a[3+i*2]);if(h<0||l<0)throw std::runtime_error("invalid hex");k.p[i]=std::uint8_t(h*16+l);}std::array<std::uint32_t,kDepth> r{};for(int d=kDepth;d>=1;--d){k.d=std::uint8_t(d);auto [it,_]=ids.emplace(k,std::uint32_t(ids.size()));r[kDepth-d]=it->second;k.p[d-1]=0;}return r;}
Trace Load(const std::string& path){std::ifstream in(path);if(!in)throw std::runtime_error("cannot open trace: "+path);std::string s;std::getline(in,s);Trace t;std::unordered_map<Key,std::uint32_t,Hash> ids;while(std::getline(in,s)){auto a=s.find(','),b=s.find(',',a+1);if(a==std::string::npos||b==std::string::npos)continue;Tx x;auto from=Intern(std::string_view(s).substr(a+1,b-a-1),ids);std::copy(from.begin(),from.end(),x.n.begin());x.paths=1;std::string_view to(s.data()+b+1,s.size()-b-1);if(to!="\"\""){auto y=Intern(to,ids);std::copy(y.begin(),y.end(),x.n.begin()+kDepth);x.paths=2;}t.tx.push_back(x);}t.keys=ids.size();return t;}
Opt Parse(int argc,char**argv){Opt o;for(int i=1;i<argc;++i){std::string a=argv[i];auto v=[&]{if(++i>=argc)throw std::runtime_error("missing value");return std::string(argv[i]);};if(a=="--algorithm")o.alg=v();else if(a=="--trace")o.path=v();else if(a=="--cache-size")o.cap=std::stoull(v());else if(a=="--capacity-ratio")o.ratio=std::stod(v());else if(a=="--history-retention")o.retention=std::stod(v());else if(a=="--self-test")o.self=true;else if(a=="--layer-protection")(void)v();else throw std::runtime_error("unknown argument: "+a);}return o;}
template<class C> Stats Replay(const Trace&t,C&c){Stats s;auto st=std::chrono::steady_clock::now();for(const auto&x:t.tx){for(int p=0;p<x.paths;++p)for(int i=0;i<kDepth;++i){auto n=x.n[p*kDepth+i];++s.req;if(c.Access(n,kDepth-i))++s.hit;else{++s.miss;c.Admit(n,kDepth-i);}}c.FinishBlock();}s.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-st).count();s.evict=c.counters().evictions;return s;}
Stats S3(const Trace&t,std::size_t cap,double ghost,bool ag,bool al,bool depth=false,bool lease=false){S3FifoBaseline c(cap,.1,ghost,2);if(depth)c.EnableDepthAdmission(kDepth,5);if(lease)c.EnableProofLease(8.0);if(ag)c.EnableAdaptiveGhost();if(al)c.EnableAdaptiveLease();Stats s;auto st=std::chrono::steady_clock::now();for(const auto&x:t.tx){for(int p=0;p<x.paths;++p)for(int i=0;i<kDepth;++i){const int d=kDepth-i;++s.req;if(c.Get(x.n[p*kDepth+i]))++s.hit;else{++s.miss;c.PutOnMiss(x.n[p*kDepth+i],d);s.evict+=c.TakeEvictedKeys().size();}}c.FinishVersion();}s.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-st).count();return s;}
void Out(const std::string&n,std::size_t c,const Stats&s){std::cout<<n<<','<<c<<','<<s.req<<','<<s.hit<<','<<s.miss<<','<<std::setprecision(12)<<(s.req?double(s.hit)/s.req:0)<<','<<s.ms<<','<<s.evict<<'\n';}
void Usage(const char*p){std::cerr<<"Usage: "<<p<<" --trace PATH (--cache-size N | --capacity-ratio R) [--algorithm prd|prd_adaptive_ghost|prd_adaptive_lease|prd_null_depth|prd_pure_depth|prd_cold_fifo|lsbt_discrete|lsbt_consecutive|s3fifo|s3fifo_ghost8|s3fifo_depth|s3fifo_lease|s3fifo_depth_lease|s3fifo_adaptive_ghost|s3fifo_adaptive_lease|s3fifo_adaptive_ghost_lease]\n";}
}
int main(int argc,char**argv){try{Opt o=Parse(argc,argv);if(o.self){PRD p(4,2);p.Access(1,1);p.Admit(1,1);if(!p.Access(1,1))throw std::runtime_error("self-test");std::cout<<"path-runner self-tests: PASS\n";return 0;}if(o.path.empty()||((o.cap==0)==(o.ratio<=0)))throw std::runtime_error("trace and exactly one capacity are required");Trace t=Load(o.path);std::size_t c=o.cap?o.cap:std::max<std::size_t>(1,std::size_t(t.keys*o.ratio));std::cout<<"algorithm,capacity,requests,hits,misses,hit_ratio,elapsed_ms,evictions\n";
if(o.alg=="prd"||o.alg=="lsbt_ghost_hot_turnover8_adaptive_ghost_lease"){PRD x(t.keys,c,kDepth,o.retention);Out("PRD",c,Replay(t,x));}else if(o.alg=="prd_adaptive_ghost"||o.alg=="lsbt_ghost_hot_turnover8_adaptive_ghost"){PRDAdaptiveGhost x(t.keys,c,kDepth,o.retention);Out("PRDAdaptiveGhost",c,Replay(t,x));}else if(o.alg=="prd_adaptive_lease"||o.alg=="lsbt_ghost_hot_turnover8_adaptive_lease"){PRDAdaptiveLease x(t.keys,c,kDepth,o.retention);Out("PRDAdaptiveLease",c,Replay(t,x));}else if(o.alg=="prd_null_depth"||o.alg=="lsbt_ghost_hot_turnover8_adaptive_ghost_lease_null_depth"){PRDNullDepth x(t.keys,c,kDepth,o.retention);Out("PRDNullDepth",c,Replay(t,x));}else if(o.alg=="prd_pure_depth"||o.alg=="lsbt_ghost_hot_turnover8_adaptive_ghost_lease_pure_depth"){PRDPureDepth x(t.keys,c,kDepth,o.retention);Out("PRDPureDepth",c,Replay(t,x));}else if(o.alg=="prd_cold_fifo"||o.alg=="lsbt_ghost_hot_turnover8_adaptive_ghost_lease_cold_fifo"){PRDColdFifo x(t.keys,c,kDepth,o.retention);Out("PRDColdFifo",c,Replay(t,x));}else if(o.alg=="lsbt_discrete"){LSBTdiscrete x(t.keys,c,kDepth,o.retention);Out("LSBTdiscrete",c,Replay(t,x));}else if(o.alg=="lsbt_consecutive"){LSBTconsecutive x(t.keys,c,kDepth,o.retention);Out("LSBTconsecutive",c,Replay(t,x));}else if(o.alg=="s3fifo"){Out("S3FIFO",c,S3(t,c,.9,false,false));}else if(o.alg=="s3fifo_ghost8"){Out("S3FIFOGhost8",c,S3(t,c,8,false,false));}else if(o.alg=="s3fifo_depth"){Out("S3FIFODepth",c,S3(t,c,.9,false,false,true,false));}else if(o.alg=="s3fifo_lease"){Out("S3FIFOLease",c,S3(t,c,.9,false,false,false,true));}else if(o.alg=="s3fifo_depth_lease"){Out("S3FIFODepthLease",c,S3(t,c,.9,false,false,true,true));}else if(o.alg=="s3fifo_adaptive_ghost"){Out("S3FIFOAdaptiveGhost",c,S3(t,c,8,true,false));}else if(o.alg=="s3fifo_adaptive_lease"){Out("S3FIFOAdaptiveLease",c,S3(t,c,8,false,true));}else if(o.alg=="s3fifo_adaptive_ghost_lease"){Out("S3FIFOAdaptiveGhostLease",c,S3(t,c,8,true,true));}else throw std::runtime_error("unsupported algorithm");return 0;}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<'\n';Usage(argv[0]);return 1;}}
