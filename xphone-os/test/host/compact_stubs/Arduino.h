#pragma once
#include <cstdint>
#include <cstdio>
#include <cstddef>
inline uint8_t pgm_read_byte(const uint8_t* p){return *p;}
struct TestSerial { template<class... T> void printf(const char* f,T... args){ std::printf(f,args...); } };
inline TestSerial Serial;
struct TestESP { unsigned getFreeHeap(){return 1024*1024;} };
inline TestESP ESP;
