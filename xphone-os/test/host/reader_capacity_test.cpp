// Deterministic failures in real FbpBook allocations and dictionary reads.
#include "../../src/reader/FbpBook.h"
#include "../../src/Gfx.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <string>
static std::map<void*,size_t> allocations;
static size_t liveBytes, peakBytes, attempts, failSize;
static bool failAll;
extern "C" void* fbp_test_malloc(size_t n) {
 ++attempts;
 if(failAll || (failSize && failSize==n)) return nullptr;
 void* p=std::malloc(n);
 if(p){allocations[p]=n;liveBytes+=n;if(liveBytes>peakBytes)peakBytes=liveBytes;}
 return p;
}
extern "C" void fbp_test_free(void* p) {
 if(!p)return;
 auto it=allocations.find(p);assert(it!=allocations.end());liveBytes-=it->second;allocations.erase(it);std::free(p);
}
extern "C" uint32_t uzlib_crc32(const void*, unsigned, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_adler32(const void*, unsigned, uint32_t) { std::abort(); }
static uint32_t u32(const unsigned char* p){return fc_u32(p);}
struct Profile{uint16_t w,h,px;uint32_t page,table,dict;uint64_t dictOff;};
static std::vector<Profile> profiles(const char* path){
 FILE* f=fopen(path,"rb");assert(f);std::vector<unsigned char>b;
 fseek(f,0,SEEK_END);b.resize(ftell(f));rewind(f);assert(fread(b.data(),1,b.size(),f)==b.size());fclose(f);
 std::vector<Profile> ps;
 for(unsigned i=0;i<u32(b.data()+24);++i){auto p=b.data()+80+48*i;uint64_t off;memcpy(&off,p+24,8);
  uint32_t dict=u32(p+32);ps.push_back({fc_u16(p),fc_u16(p+2),fc_u16(p+4),dict+u32(p+36)+1,p[47]==2?u32(b.data()+off+dict+4)*8:0,dict,off});}
 return ps;
}
static void drawWords(reader::FbpBook& book,Gfx& gfx,unsigned page){
 gfx.clear();assert(book.renderPage(gfx,page));assert(book.hasWordBoxes());
 reader::FbpBook::WordBox words[512];auto n=book.pageWords(words,512);assert(n>0);
 char text[256];book.wordText(words[0],text,sizeof(text));assert(strlen(text)>0);
}
static void test(const char* path){
 peakBytes=0;
 auto ps=profiles(path);assert(ps.size()==3);auto small=ps.front(),large=ps.back();
 uint32_t maxPage=0,maxTable=0;for(auto p:ps){if(p.page>maxPage)maxPage=p.page;if(p.table>maxTable)maxTable=p.table;}
 EInkDisplay display;Gfx gfx(display);assert(gfx.begin());
 assert(gfx.width()==large.w && gfx.height()==large.h);
 printf("Native display=%ux%u logical portrait=%ux%u\n",hostDisplayWidth,hostDisplayHeight,gfx.width(),gfx.height());
 size_t measuredPeak=0;
 { reader::FbpBook book;assert(book.open(path));assert(book.selectProfile(large.w,large.h,large.px));
  assert(book.residentBytes()==maxPage+maxTable);assert(liveBytes==book.residentBytes());
  drawWords(book,gfx,1);measuredPeak=peakBytes;
  printf("%s reserve=%u selectedMinimum=%u added=%u renderPeak=%u measuredAllocPeak=%zu\n",path,book.residentBytes(),large.page+large.table,book.residentBytes()-large.page-large.table,book.lastPeakBytes(),measuredPeak);
  auto before=attempts;failAll=true;uint16_t page=1;
  for(int i=0;i<12;++i){assert(book.stepSize(1,page,&page));assert(book.profileSelected());}
  failAll=false;assert(attempts==before);drawWords(book,gfx,page);
 } assert(liveBytes==0);
 // Maximum page reservation fails; the smaller requested profile still opens.
 { reader::FbpBook book;failSize=maxPage;assert(book.open(path));assert(book.selectProfile(large.w,large.h,large.px));
  assert(book.pxSize()==large.px);assert(book.residentBytes()==large.page+large.table);drawWords(book,gfx,1);
  std::vector<unsigned char> before(display.bytes,display.bytes+sizeof(display.bytes));
  uint16_t page=1;assert(!book.stepSize(1,1,&page));assert(page==1 && book.profileSelected() && book.pxSize()==large.px);
  assert(!book.hasWordBoxes());drawWords(book,gfx,1);assert(!memcmp(before.data(),display.bytes,before.size()));
  failSize=0;
 } assert(liveBytes==0);
 // Maximum table reservation fails atomically; a smaller table can still open.
 if(maxTable>small.table){
  reader::FbpBook book;failSize=maxTable;assert(book.open(path));assert(book.selectProfile(small.w,small.h,small.px));
  assert(book.residentBytes()==small.page+small.table);assert(liveBytes==book.residentBytes());drawWords(book,gfx,1);
  uint16_t page=1;assert(!book.stepSize(1,1,&page));assert(book.profileSelected() && book.pxSize()==small.px);drawWords(book,gfx,1);failSize=0;
 } assert(liveBytes==0);
 // A dictionary failure after metadata changed restores the old profile.
 { reader::FbpBook book;assert(book.open(path));assert(book.selectProfile(large.w,large.h,large.px));drawWords(book,gfx,1);
  std::vector<unsigned char> before(display.bytes,display.bytes+sizeof(display.bytes));
  testFailReadSize=small.dict;testFailReadCount=1;uint16_t page=1;
  assert(!book.stepSize(1,1,&page));assert(book.profileSelected() && book.pxSize()==large.px);
  drawWords(book,gfx,1);assert(!memcmp(before.data(),display.bytes,before.size()));
  // Both the target and rollback reads fail: never advertise a valid profile.
  testFailReadCount=2;assert(!book.stepSize(1,1,&page));assert(!book.profileSelected());assert(!book.hasWordBoxes());assert(!book.renderPage(gfx,1));
  testFailReadCount=0;assert(book.selectProfile(large.w,large.h,large.px));drawWords(book,gfx,1);
  // Earlier atlas-header failure must not leave old word pointers usable.
  testFailReadSize=1;testFailReadCount=2;assert(!book.stepSize(1,1,&page));
  assert(!book.profileSelected());assert(!book.hasWordBoxes());
  reader::FbpBook::WordBox word;assert(book.pageWords(&word,1)==0);assert(book.lineCidCount()==0);
  testFailReadCount=0;assert(book.selectProfile(large.w,large.h,large.px));drawWords(book,gfx,1);
 } assert(liveBytes==0);
 puts("PASS: capacity reuse (12 steps, zero allocation attempts), page/table fallback, rollback pixels/words, explicit rollback failure, no leaks");
}
static std::string wordState(reader::FbpBook& book) {
 reader::FbpBook::WordBox words[512];auto count=book.pageWords(words,512);assert(count>0 && count<512);
 std::string result;
 for(unsigned i=0;i<count;++i){
  auto w=words[i];char fields[100],text[256];
  snprintf(fields,sizeof(fields),"%d,%d,%u,%u,%u:",w.x,w.w,w.line,w.flags,w.textLen);
  book.wordText(w,text,sizeof(text));result+=fields;result.append(text,w.textLen);result+=';';
 }
 for(unsigned i=0;i<book.lineCidCount();++i){char line[100];snprintf(line,sizeof(line),"%u,%d;",book.lineCid(i),book.lineBaseline(i));result+=line;}
 return result;
}
static void orientationTest(const char* path) {
 auto ps=profiles(path);assert(ps.size()==6);auto portrait=ps[2],landscape=ps[5];
 assert(portrait.w<portrait.h && landscape.w>landscape.h);
 EInkDisplay display;Gfx gfx(display);assert(gfx.begin());assert(gfx.width()==portrait.w && gfx.height()==portrait.h);
 {reader::FbpBook book;assert(book.open(path));assert(book.selectProfile(portrait.w,portrait.h,portrait.px));drawWords(book,gfx,1);
  const unsigned pages=book.pageCount();uint32_t cid,sid;assert(book.pageFirstCidPublic(1,&cid)&&book.pageFirstSidPublic(1,&sid));
  const std::vector<unsigned char> before(display.bytes,display.bytes+sizeof(display.bytes));const std::string words=wordState(book);
  for(unsigned i=3;i<6;++i){assert(ps[i].dict>0);testFailReadOffsets[testFailReadOffsetsCount++]=ps[i].dictOff;}
  gfx.setOrientation(Gfx::Orient::Landscape);
  assert(!book.selectProfileFresh(landscape.w,landscape.h));gfx.setOrientation(Gfx::Orient::Portrait);
  assert(book.profileSelected() && book.pxSize()==portrait.px && book.pageCount()==pages);assert(!book.hasWordBoxes());
  drawWords(book,gfx,1);assert(!memcmp(before.data(),display.bytes,before.size()));assert(words==wordState(book));
  uint32_t restoredCid,restoredSid;assert(book.pageFirstCidPublic(1,&restoredCid)&&book.pageFirstSidPublic(1,&restoredSid));assert(cid==restoredCid&&sid==restoredSid);
  // Fail the old profile too: the geometry returns but its contents are invalid.
  testFailReadOffsets[testFailReadOffsetsCount++]=portrait.dictOff;
  assert(!book.selectProfileFresh(landscape.w,landscape.h));assert(!book.profileSelected());assert(!book.hasWordBoxes());assert(book.lineCidCount()==0);assert(!book.renderPage(gfx,1));
  testFailReadOffsetsCount=0;
  assert(book.selectProfile(portrait.w,portrait.h,portrait.px));drawWords(book,gfx,1);assert(words==wordState(book));
  assert(book.selectProfileFresh(landscape.w,landscape.h));gfx.setOrientation(Gfx::Orient::Landscape);drawWords(book,gfx,1);
  assert(book.selectProfileFresh(portrait.w,portrait.h));gfx.setOrientation(Gfx::Orient::Portrait);drawWords(book,gfx,1);
  assert(!memcmp(before.data(),display.bytes,before.size()));assert(words==wordState(book));
 }assert(liveBytes==0);
 printf("PASS: %s orientation rollback restores all pixels/words/anchors; failed rollback invalidates; clean roundtrip exact; no leaks\n",path);
}
int main(int argc,char**argv){
 assert(argc==3);
 if(!strcmp(argv[1],"--orientation")){orientationTest(argv[2]);return 0;}
 if(!strcmp(argv[1],"--reject-page")){
  {reader::FbpBook book;assert(book.open(argv[2]));assert(book.selectProfile(528,792,34));
   EInkDisplay display;Gfx gfx(display);assert(gfx.begin());assert(!book.renderPage(gfx,1));}
  assert(liveBytes==0);puts("PASS: selected profile raw bound enforced inside larger reserved capacity");return 0;
 }
 if(!strcmp(argv[1],"--reject")){
  {reader::FbpBook book;if(book.open(argv[2]))assert(!book.selectProfile(528,792,34));}
  assert(liveBytes==0);assert(peakBytes<=73729);puts("PASS: malformed profile rejected within allocation caps");return 0;
 }
 test(argv[1]);test(argv[2]);
}
