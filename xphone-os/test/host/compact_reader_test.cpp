/* Real firmware FbpBook + Gfx + uzlib, backed by host files. */
#include "../../src/reader/FbpBook.h"
#include "../../src/Gfx.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
// Raw FBP inflation never calls wrapped-stream checksums.
extern "C" uint32_t uzlib_crc32(const void*, unsigned, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_adler32(const void*, unsigned, uint32_t) { std::abort(); }
int main(int argc,char** argv) {
 assert(argc==4); unsigned profiles=(unsigned)atoi(argv[3]);
 uint32_t pages=0;
 for(unsigned pi=0;pi<profiles;pi++) {
  reader::FbpBook a,b;assert(a.open(argv[1]) && b.open(argv[2]));
  uint16_t aw,ah,bw,bh;assert(a.benchProfile(pi,&aw,&ah)&&b.benchProfile(pi,&bw,&bh));
  assert(aw==bw && ah==bh && a.pageCount()==b.pageCount());
  EInkDisplay da,db;Gfx ga(da),gb(db);assert(ga.begin()&&gb.begin());
  ga.setOrientation(aw>ah?Gfx::Orient::Landscape:Gfx::Orient::Portrait);gb.setOrientation(aw>ah?Gfx::Orient::Landscape:Gfx::Orient::Portrait);
  for(unsigned page=0;page<a.pageCount();page++) {
   // A coprime affine permutation exercises backwards and random seeks.
   unsigned pg=(page*97u)%a.pageCount();if(a.pageCount()%97==0)pg=a.pageCount()-1-page;
   uint32_t ca,cb;assert(a.benchBodyCrc(pg,&ca)&&b.benchBodyCrc(pg,&cb)&&ca==cb);
   ga.clear();gb.clear();assert(a.renderPage(ga,pg)&&b.renderPage(gb,pg));
   assert(!memcmp(da.bytes,db.bytes,sizeof(da.bytes)));
   reader::FbpBook::WordBox wa[4096],wb[4096];
   auto na=a.pageWords(wa,4096),nb=b.pageWords(wb,4096);assert(na==nb);
   for(unsigned w=0;w<na;w++) {
    assert(wa[w].x==wb[w].x && wa[w].w==wb[w].w && wa[w].line==wb[w].line && wa[w].flags==wb[w].flags && wa[w].textLen==wb[w].textLen);
    char ta[256],tb[256];a.wordText(wa[w],ta,256);b.wordText(wb[w],tb,256);assert(!memcmp(ta,tb,wa[w].textLen));
   }
   assert(a.lineCidCount()==b.lineCidCount());
   for(unsigned l=0;l<a.lineCidCount();l++)assert(a.lineCid(l)==b.lineCid(l)&&a.lineBaseline(l)==b.lineBaseline(l));
   uint32_t ma[64],mb[64];auto nma=a.pageMarkCids(ma,64),nmb=b.pageMarkCids(mb,64);assert(nma==nmb&&!memcmp(ma,mb,nma*4));
   pages++;
  }
 }
 printf("Firmware direct decoder: %u pages, body CRC, native framebuffer, word boxes/text, line anchors, note marks all exact.\n",pages);
}
