#include "../../tools/FloweRawReceive.h"
#include <cassert>
#include <cstring>
#include <iostream>
static int polls=0;
namespace transfer_sync {bool pollControls(){return ++polls==3;}}
struct Client {
 bool data=true;int fd(){return 1;}int available(){return data?73:0;}
 int read(uint8_t* p,size_t n){memset(p,1,n);return n;}
 bool connected(){return true;}
};
int main(){
 Client c;uint8_t out[1024];unsigned time=0;
 auto now=[&]{return time;};auto wait=[&](unsigned ms){time+=ms;};
 auto got=flowe_raw::receive(c,out,sizeof(out),now,wait,[](int){return 1;});
 assert(got.size==146 && !strcmp(got.error,"cancelled"));
 polls=0;c.data=false;
 got=flowe_raw::receive(c,out,sizeof(out),now,wait,[](int){return 1;});
 assert(got.size==0 && !strcmp(got.error,"cancelled") && time==4);
 std::cout<<"PASS: cancel during active and stalled raw body reads\n";
}
