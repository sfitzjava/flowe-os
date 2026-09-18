#include "../../tools/FloweRawReceive.h"
#include "../../src/net/UploadPublication.h"
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstring>
#include <iostream>
struct Client {
  std::vector<uint8_t> bytes; size_t position=0; int maxRead=73;
  bool live=true; bool closed=false; bool readError=false;
  int fd() { return closed ? -1 : 1; }
  int available() { return bytes.size()-position; }
  bool connected() { return live; }
  int read(uint8_t* out,size_t n) {
    if(readError) return -1;
    n=std::min(n,size_t(maxRead)); memcpy(out,bytes.data()+position,n); position+=n; return n;
  }
};
struct Storage {
  std::map<std::string,std::string> files;
  std::set<unsigned> failRenames; unsigned renames=0;
  bool exists(const char* p) { return files.count(p); }
  bool remove(const char* p) { return files.erase(p); }
  bool rename(const char* a,const char* b) {
    if(failRenames.count(++renames) || !exists(a) || exists(b)) return false;
    files[b]=files[a]; files.erase(a); return true;
  }
};
int main() {
  uint32_t n;
  for(auto bad:{"", "0", "-1", "+1", "1x", "1,1", "67108865", "4294967296", "999999999999999999999"}) assert(!flowe_raw::parseLength(bad,strlen(bad),n));
  assert(flowe_raw::parseLength("67108864",8,n) && n==67108864);
  for(size_t length:{size_t(1),size_t(4099),size_t(8197)}) {
    Client c; for(size_t i=0;i<length;++i)c.bytes.push_back(i%251);
    c.live=false; // Drain the entire NetworkClient buffer after FIN.
    uint32_t clock=0; size_t total=0; std::vector<uint8_t> result;
    while(total<length){ uint8_t buf[1436]; auto r=flowe_raw::receive(c,buf,std::min(length-total,sizeof(buf)),[&]{return clock;},[&](unsigned ms){clock+=ms;},[](int){return 0;}); assert(!r.error); result.insert(result.end(),buf,buf+r.size);total+=r.size; }
    assert(result==c.bytes && clock==0);
  }
  for(bool eof:{false,true}) {
    Client c; c.bytes.resize(1000,17); uint8_t buf[1436]; uint32_t clock=UINT32_MAX-200;
    const auto start=clock;
    auto r=flowe_raw::receive(c,buf,sizeof(buf),[&]{return clock;},[&](unsigned ms){clock+=ms;},[&](int){return eof?0:1;});
    assert(r.error && r.size==1000 && c.position==1000);
    assert(uint32_t(clock-start)==(eof?0:5000));
  }
  { Client c; c.closed=true; uint8_t b; auto r=flowe_raw::receive(c,&b,1,[]{return 0;},[](unsigned){assert(false);},[](int){assert(false);return 1;}); assert(r.error && !r.size); }
  // Rename failure never removes the old bytes; immediate retry succeeds.
  for(unsigned fail:{1u,2u}) {
    Storage s; s.files={{"final","old"},{"part","new"}};s.failRenames={fail};
    assert(!flowe_upload::publish(s,"part","final","backup"));
    assert(s.files["final"]=="old");s.failRenames.clear();
    assert(flowe_upload::publish(s,"part","final","backup"));assert(s.files["final"]=="new");
  }
  { Storage s;s.files={{"final","old"},{"part","new"}};s.failRenames={2,3};
    assert(!flowe_upload::publish(s,"part","final","backup"));assert(s.files["backup"]=="old");
    s.failRenames.clear();assert(flowe_upload::recover(s,"final","backup"));assert(s.files["final"]=="old"); }
  // Reset before promotion; reset after promotion.
  for(bool promoted:{false,true}) { Storage s;s.files={{"backup","old"},{"part","new"}};if(promoted)s.files["final"]="new";
    assert(flowe_upload::recover(s,"final","backup"));assert(s.files["final"]==(promoted?"new":"old")); }
  // Exercise the production END gate with short write, close failure and a
  // previously failed/cancelled body. Prior bytes remain and retry works.
  for(unsigned failure=0;failure<4;++failure) {
    Storage sd; sd.files={{"final","old"},{"part","new"}};
    unsigned closes=0,publishes=0;
    auto result=flowe_upload::finish(failure==3,
        [&]{return failure!=1;}, [&]{++closes;return failure!=2;},
        [&]{++publishes;return flowe_upload::publish(sd,"part","final","backup");});
    assert(result==(failure==0)); assert(closes==1);
    assert(publishes==(failure==0?1u:0u));
    assert(sd.files["final"]==(failure==0?"new":"old"));
    if(failure) assert(flowe_upload::finish(false,[]{return true;},[]{return true;},
        [&]{return flowe_upload::publish(sd,"part","final","backup");}));
  }
  { Client c; c.bytes={1};c.readError=true;uint8_t b;
    auto r=flowe_raw::receive(c,&b,1,[]{return 0;},[](unsigned){assert(false);},[](int){return 1;});assert(r.error&&!r.size); }
  { Client c;uint8_t b;auto r=flowe_raw::receive(c,&b,1,[]{return 0;},[](unsigned){assert(false);},[](int){return -1;});assert(r.error&&!r.size); }
  std::cout<<"raw receive, framing, publication tests passed\n";
}
