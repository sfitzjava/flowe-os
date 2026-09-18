#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static std::map<void*,size_t> live;
static unsigned reallocs=0;
static bool failNextRealloc=false, forceMove=false;
static void* trackMalloc(size_t n){void* p=std::malloc(n);assert(p);live[p]=n;return p;}
static void trackFree(void* p){if(p){assert(live.count(p));live.erase(p);}std::free(p);}
static void* trackRealloc(void* p,size_t n){
  ++reallocs;if(failNextRealloc){failNextRealloc=false;return nullptr;}
  if(p)assert(live.count(p));
  const size_t old=p?live[p]:0;
  if(forceMove && p){void* q=std::malloc(n);assert(q);memcpy(q,p,std::min(old,n));trackFree(p);live[q]=n;return q;}
  // Save the key before realloc can release the allocation.
  const uintptr_t key=reinterpret_cast<uintptr_t>(p);
  void* q=std::realloc(p,n);assert(q);if(key)live.erase(reinterpret_cast<void*>(key));live[q]=n;return q;
}
#define ESP_IDF_VERSION_MAJOR 5
#define lwip_ioctl ioctl
#define log_e(...) ((void)0)
#define log_d(...) ((void)0)
#define log_i(...) ((void)0)
#define malloc trackMalloc
#define realloc trackRealloc
#define free trackFree
#include "PinnedNetworkClient.inc"
#undef malloc
#undef realloc
#undef free
struct Pair {
  int fd[2]; std::unique_ptr<NetworkClient> client;
  Pair(){assert(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);int n=65536;assert(setsockopt(fd[1],SOL_SOCKET,SO_SNDBUF,&n,sizeof(n))==0);assert(setsockopt(fd[0],SOL_SOCKET,SO_RCVBUF,&n,sizeof(n))==0);client.reset(new NetworkClient(fd[0]));}
  void send(const std::vector<uint8_t>& b){assert(::send(fd[1],b.data(),b.size(),0)==static_cast<ssize_t>(b.size()));}
  ~Pair(){client.reset();::close(fd[1]);assert(live.empty());}
};
static std::vector<uint8_t> pattern(size_t n){std::vector<uint8_t>b(n);for(size_t i=0;i<n;++i)b[i]=static_cast<uint8_t>((i*13+i/1436)%251);return b;}
static void drain(Pair& p,const std::vector<uint8_t>& expected){
  size_t total=0;
  while(total<expected.size()){
    uint8_t b[1436];
    const size_t wanted=std::min(sizeof(b),expected.size()-total);
    const int n=p.client->read(b,wanted);
    assert(n>0 && static_cast<size_t>(n)<=wanted);
    assert(!memcmp(b,expected.data()+total,n));total+=n;
  }
}

int main(){
  for(size_t growth:{size_t(2872),size_t(5744)})
  for(bool fail:{false,true})
  for(size_t bodySize:{size_t(1435),size_t(1436),size_t(1437),size_t(2872),size_t(5745),size_t(16385)}){
    Pair p;auto body=pattern(bodySize);std::vector<uint8_t> wire(387,'H');wire.insert(wire.end(),body.begin(),body.end());p.send(wire);assert(shutdown(p.fd[1],SHUT_WR)==0);
    uint8_t header[387];assert(p.client->read(header,sizeof(header))==int(sizeof(header)));
    assert(live.size()==1 && live.begin()->second==1436);
    // Initial1436-byte refill leaves exactly1049 body bytes prefetched.
    void* old=live.begin()->first;const auto before=reallocs;
    forceMove=true;failNextRealloc=fail;
    assert(p.client->floweGrowReadBuffer(growth)==!fail);
    assert(reallocs==before+1 && live.size()==1);
    if(fail)assert(live.count(old) && live.begin()->second==1436);
    else assert(!live.count(old) && live.begin()->second==growth);
    drain(p,body); // All prefetched and later socket bytes survive growth/FIN.
  }
  // Null-buffer growth is eager, allowing OOM to keep lazy1436-byte fallback.
  for(bool fail:{false,true}){
    Pair p;assert(live.empty());failNextRealloc=fail;
    assert(p.client->floweGrowReadBuffer(2872)==!fail);
    assert(live.size()==(fail?0u:1u));
    auto body=pattern(8197);p.send(body);assert(shutdown(p.fd[1],SHUT_WR)==0);drain(p,body);
    assert(live.begin()->second==(fail?1436u:2872u));
  }
  // Repeated/smaller sizes never shrink or allocate; bounds are enforced.
  {Pair p;assert(p.client->floweGrowReadBuffer(2872));void* original=live.begin()->first;auto before=reallocs;
    assert(p.client->floweGrowReadBuffer(2872));assert(p.client->floweGrowReadBuffer(1436));assert(p.client->floweGrowReadBuffer(1));
    assert(!p.client->floweGrowReadBuffer(0));assert(!p.client->floweGrowReadBuffer(5745));
    assert(reallocs==before && live.count(original));
    assert(p.client->floweGrowReadBuffer(5744));assert(live.begin()->second==5744);
    p.client->stop();before=reallocs;assert(!p.client->floweGrowReadBuffer(2872) && reallocs==before && live.empty());}
  // Data consumed after the initial refill can leave_pos==_fill; grow keeps
  // that state valid and the next refill resets it normally.
  {Pair p;auto body=pattern(1436);p.send(body);uint8_t out[1436];assert(p.client->read(out,1436)==1436);assert(!memcmp(out,body.data(),1436));
    assert(p.client->floweGrowReadBuffer(5744));auto next=pattern(5745);p.send(next);assert(shutdown(p.fd[1],SHUT_WR)==0);drain(p,next);}
  // The private grow rejects an already failed Rx; no allocation/recovery.
  {Pair p;::close(p.client->fd());uint8_t b;assert(p.client->_rxBuffer->read(&b,1)<0);auto before=reallocs;
    assert(!p.client->_rxBuffer->grow(2872) && reallocs==before);p.client->stop();}
  assert(live.empty());
  std::cout<<"PASS: exact prefetched1049B preservation with forced relocation, FIN,1436 boundaries, OOM preservation, null-buffer fallback, repeat/no-shrink, cap, stopped/failed client, full cleanup\n";
}
