#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#define private public
#include "../../../../freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h"
#undef private
#include "../../../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/PanelDriver.h"
#include "esp_heap_caps.h"
using namespace freeink;
static size_t allocations=0, frees=0, allocationSize=0;
static bool failAllocation=false;
static FreeInkDisplay* owner=nullptr;
void* heap_caps_malloc(size_t n,unsigned) {allocationSize=n;if(failAllocation)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p) {assert(owner->getFrameBuffer()==nullptr);
#if FREEINK_FB_RELEASABLE
assert(owner->frameBuffer0==nullptr);
#endif
++frees;free(p);}
void EpdBus::begin(const EpdPins&,uint32_t,BusyPolarity,int8_t,int8_t) {}
struct Driver : PanelDriver {
 int paints=0; PanelGeometry geom{800,480,100,48000};
 uint32_t spiHz() const override{return 1;}
 BusyPolarity busyPolarity() const override{return BusyPolarity::ActiveHigh;}
 PanelGeometry geometry() const override{return geom;}
 void begin(EpdBus&) override{}
 void deepSleep(EpdBus&) override{}
 void display(EpdBus&,const uint8_t* fb,const uint8_t*,RefreshMode,bool) override {assert(fb);++paints;}
};
int main() {
 FreeInkDisplay d(0,0,0,0,0,0);owner=&d;Driver driver;d._driver=&driver;d.begin();
 assert(d.getFrameBuffer());d.displayBuffer();assert(driver.paints==1);
#if FREEINK_FB_RELEASABLE
 assert(allocationSize==48000);
 for(int i=0;i<4;++i) {
  assert(d.releaseFramebufferForSync());assert(!d.releaseFramebufferForSync());assert(frees==size_t(i+1));
  assert(!d.getFrameBuffer());d.clearScreen();d.setFramebuffer(nullptr);d.drawImage(nullptr,0,0,8,1);
  d.displayBuffer();d.displayWindow(0,0,8,8);d.displayWindowFlash(0,0,8,8);
  d.displayGrayBuffer();d.displayGrayscaleBase();d.copyGrayscaleBuffers(nullptr,nullptr);
  d.preconditionGrayscale();d.cleanupGrayscaleBuffers(nullptr);assert(driver.paints==1);
  failAllocation=true;assert(!d.restoreFramebufferAfterSync());assert(!d.getFrameBuffer());
  failAllocation=false;assert(d.restoreFramebufferAfterSync());
  assert(d.restoreFramebufferAfterSync());assert(allocations==size_t(i+2));
  for(size_t j=0;j<48000;++j)assert(d.getFrameBuffer()[j]==255);
 }
 // Re-init with a different active panel must resize, not overrun X4 RAM.
 driver.geom={792,528,99,52272};d.begin();assert(allocationSize==52272);
 assert(d.releaseFramebufferForSync());
#else
 auto* original=d.getFrameBuffer();assert(!d.releaseFramebufferForSync());assert(d.getFrameBuffer()==original);
 assert(d.restoreFramebufferAfterSync());d.clearScreen(0);assert(original[0]==0);
#if FREEINK_FB_PSRAM
 free(d.frameBuffer0);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
 free(d.frameBuffer1);
#endif
#endif
#endif
 std::cout<<"PASS: SDK release/restore, OOM, repeat ownership, static/PSRAM policy\n";
}
