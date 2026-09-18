#pragma once
#include <cstdint>
#include <cstring>
#if defined(FLOWE_TEST_DISPLAY_X3)
inline constexpr int hostDisplayWidth = 792, hostDisplayHeight = 528;
#else
inline constexpr int hostDisplayWidth = 800, hostDisplayHeight = 480;
#endif
class EInkDisplay {
public:
 enum RefreshMode {FULL_REFRESH, HALF_REFRESH, FAST_REFRESH};
 uint8_t bytes[hostDisplayWidth * hostDisplayHeight / 8]; uint8_t* fb=bytes;
 uint8_t* getFrameBuffer() const {return fb;}
 size_t getBufferSize() const {return sizeof(bytes);}
 int getDisplayWidth() const {return hostDisplayWidth;}
 int getDisplayHeight() const {return hostDisplayHeight;}
 int getDisplayWidthBytes() const {return hostDisplayWidth / 8;}
 void clearScreen(uint8_t v=255) {if(fb) memset(fb,v,sizeof(bytes));}
 void displayBuffer(RefreshMode) {}
 void displayWindow(int,int,int,int) {}
 void displayWindowFlash(int,int,int,int) {}
 bool releaseFramebufferForSync(){fb=nullptr;return true;}
 bool restoreFramebufferAfterSync(){fb=bytes;return true;}
};
