#include "TwoTimescaleCache.hpp"
#include "TwoTimescaleCacheSparse.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
struct Req{uint32_t key;uint32_t path;uint8_t depth;};
struct Trace{std::vector<Req> r;size_t key_count=0;};
Trace Load(const char* p){std::ifstream in(p);std::string line;std::getline(in,line);Trace t;t.r.reserve(8000000);uint32_t mx=0;while(std::getline(in,line)){auto a=line.find(',');auto b=line.find(',',a+1);if(a==std::string::npos||b==std::string::npos)continue;uint32_t path=std::stoul(line.substr(0,a));int d=std::stoi(line.substr(a+1,b-a-1));uint32_t k=std::stoul(line.substr(b+1));t.r.push_back({k,path,(uint8_t)d});mx=std::max(mx,k);}t.key_count=(size_t)mx+1;return t;}
template<class C> uint64_t Replay(const Trace&t,size_t cap){C c(t.key_count,cap,8);size_t L=2*cap,pos=0;uint32_t lp=UINT32_MAX;for(auto&q:t.r){if(lp!=UINT32_MAX&&q.path!=lp)c.EndMicroUnit();lp=q.path;if(pos==L){c.EndEpoch();pos=0;}c.Access(q.key,q.depth,true);++pos;}if(pos)c.EndEpoch();return c.stats().hits;}
template<class C> double One(const Trace&t,size_t cap,uint64_t&h){auto a=std::chrono::steady_clock::now();h=Replay<C>(t,cap);auto b=std::chrono::steady_clock::now();return std::chrono::duration<double,std::milli>(b-a).count();}
template<class C> double Med(const Trace&t,size_t cap,int n,uint64_t&h){std::vector<double>v;One<C>(t,cap,h);for(int i=0;i<n;i++)v.push_back(One<C>(t,cap,h));std::sort(v.begin(),v.end());return v[v.size()/2];}
int main(int argc,char**argv){if(argc<3)return 2;auto t=Load(argv[1]);size_t C=std::stoull(argv[2]);int n=argc>3?std::stoi(argv[3]):5;uint64_t a,b;double x=Med<TwoTimescaleCache>(t,C,n,a),y=Med<TwoTimescaleCacheSparse>(t,C,n,b);std::cout<<std::fixed<<std::setprecision(3)<<"C="<<C<<" req="<<t.r.size()<<" key_count="<<t.key_count<<" hits_v2="<<a<<" hits_sparse="<<b<<" v2_ms="<<x<<" sparse_ms="<<y<<" speedup="<<x/y<<"\n";}
