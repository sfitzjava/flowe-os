#include "../../src/net/ControlledWrite.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <iostream>
int main(){
 unsigned clock=0,calls=0;uint8_t bytes[4096];for(unsigned i=0;i<4096;++i)bytes[i]=i%251;
 std::vector<uint8_t> received;
 auto now=[&]{return clock;};auto pause=[&](unsigned n){clock+=n;};
 auto send=[&](const uint8_t* p,size_t n){if(++calls%3==0)return 0;int count=n>73?73:int(n);received.insert(received.end(),p,p+count);return count;};
 auto r=transfer_sync::writeControlled(bytes,sizeof(bytes),3000,send,now,pause,[]{return false;});
 assert(r.complete && r.bytes==sizeof(bytes) && !memcmp(bytes,received.data(),sizeof(bytes)));
 clock=0;r=transfer_sync::writeControlled(bytes,sizeof(bytes),3000,[](const uint8_t*,size_t){return 0;},now,pause,[&]{return clock>=20;});
 assert(!r.complete && r.bytes==0 && clock==20);
 clock=0;r=transfer_sync::writeControlled(bytes,sizeof(bytes),30,[](const uint8_t*,size_t){return 0;},now,pause,[]{return false;});
 assert(!r.complete && clock==30);
 r=transfer_sync::writeControlled(bytes,sizeof(bytes),30,[](const uint8_t*,size_t){return -1;},now,pause,[]{return false;});assert(!r.complete);
 std::cout<<"PASS: exact partial sends, blocked cancel, timeout, closed socket\n";
}
