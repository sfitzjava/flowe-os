#include "../../src/Gfx.h"
#include <cassert>
#include <cstring>
#include <iostream>
int main() {
 EInkDisplay d; Gfx gfx(d); assert(gfx.begin()); d.clearScreen();
#ifdef CHARACTERIZE_OLD_SYNC
 d.fb=nullptr; // The SDK releases ownership but Gfx retains writable storage.
 gfx.fillRect(0,0,480,800,true);
 assert(d.bytes[0]==0); std::cout<<"BASELINE: Gfx writes through its stale framebuffer cache\n";
#else
 assert(gfx.releaseFramebufferForSync());
 gfx.drawPixel(0,0,true); gfx.fillRect(0,0,480,800,true);
 const uint8_t bitmap[]={255};
 const EpdGlyph glyphs[]={{2,2,32,0,2,1,0}};
 const EpdUnicodeInterval intervals[]={{65,65,0}};
 const XpFont f{bitmap,glyphs,intervals,1,24,20};
 gfx.drawText(f,0,0,"A");
 gfx.setOrientation(Gfx::Orient::Landscape); gfx.drawText(f,0,0,"A");
 gfx.fillRect(0,0,800,480,true); gfx.drawPixel(0,0,true); gfx.invert();
 gfx.setOrientation(Gfx::Orient::Portrait);
 gfx.clear(); gfx.flush(EInkDisplay::FAST_REFRESH); gfx.flushWindow(0,0,10,10);
 for(auto b:d.bytes) assert(b==255);
 assert(gfx.restoreFramebufferAfterSync()); gfx.drawPixel(0,0,true);
 assert(d.bytes[47900]==127);
 std::cout<<"PASS: disabled drawing preserves released bytes; restored drawing works\n";
#endif
}
