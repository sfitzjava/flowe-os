// Exact production handleStatus is inserted by the Python runner.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <iostream>
std::vector<std::string> events;
bool memoryReleased=false,readyResult=true,releaseOnReady=true;
namespace transfer_sync {bool memoryReleased(){return ::memoryReleased;}}
struct Network {
 int getMode(){return 1;}
 struct Address{std::string toString(){return "192.168.4.1";}};
 Address softAPIP(){return {};}Address localIP(){return {};}
} WiFi;
constexpr int WIFI_AP=1;bool gDeviceIsX3=false;
constexpr const char* XPHONE_VERSION="test";constexpr const char* XPHONE_GIT_REV_STR="test";
struct {unsigned getFreeHeap(){events.push_back("heap");return memoryReleased?70000:22000;}} ESP;
namespace WifiCreds {constexpr unsigned kReaderIdSize=13;bool readerId(char* s,unsigned n){snprintf(s,n,"001122334455");return true;}}
namespace stallwatch {unsigned worstStallMs(){return 0;}}
struct JsonDocument {
 std::map<std::string,std::string> fields;
 JsonDocument(){events.push_back("json");}
 struct Proxy {
  std::string& dst;
  void operator=(const char* s){dst=s;}void operator=(const std::string& s){dst=s;}
  void operator=(unsigned n){dst=std::to_string(n);}void operator=(int n){dst=std::to_string(n);}
 };
 Proxy operator[](const char* key){return {fields[key]};}
};
std::map<std::string,std::string> responseFields;
void serializeJson(JsonDocument& doc,char*,size_t){responseFields=doc.fields;events.push_back("serialize");}
struct Http {
 int code=0;bool releasedAt200=false;
 void send(int n,const char*,const char*){code=n;events.push_back("send"+std::to_string(n));if(n==200)releasedAt200=memoryReleased;}
};
struct FileTransferServer {
 bool identity=true,token=true,_verifiedContact=false;unsigned _requestCount=0;Http http;Http* _server=&http;
 bool (*prepareStatusHook)(void*)=nullptr;void* prepareStatusContext=nullptr;
 bool targetReaderOk(){return identity;}bool tokenOk(){return token;}
 void handleStatus();
};
bool prepare(void* ctx){assert(ctx==&events);events.push_back("prepare");if(readyResult && releaseOnReady)memoryReleased=true;return readyResult;}
// PRODUCTION_STATUS
void reset(FileTransferServer& server){server.identity=server.token=true;server._verifiedContact=false;server._requestCount=0;server.http={};server.prepareStatusHook=prepare;server.prepareStatusContext=&events;events.clear();responseFields.clear();memoryReleased=false;readyResult=true;releaseOnReady=true;}
int main(){
 FileTransferServer server;reset(server);
#ifdef CHARACTERIZE_READY_RACE
 server.handleStatus();assert(server.http.code==200 && !server.http.releasedAt200);
 assert(server._verifiedContact);std::cout<<"BASELINE: status200 precedes memory preparation and release\n";
#else
 server.handleStatus();assert(server.http.code==200 && server.http.releasedAt200);
 assert(events.front()=="prepare" && events[1]=="json" && events.back()=="send200");
 assert(responseFields["compactPageVersion"]=="1" && responseFields["rawUploadVersion"]=="1");
 assert(responseFields["freeHeap"]=="70000" && responseFields["readerId"]=="001122334455");
#if defined(FLOWE_SYNC_FAST_SDK)
 reset(server);releaseOnReady=false;server.handleStatus();assert(server.http.code==503 && responseFields.empty());
#endif
 reset(server);readyResult=false;server.handleStatus();assert(server.http.code==503 && !server.http.releasedAt200);
 assert((events==std::vector<std::string>{"prepare","send503"}) && responseFields.empty());
 reset(server);server.token=false;server.handleStatus();assert(!server._verifiedContact && server.http.code==200 && !memoryReleased);
 assert(responseFields.count("readerId") && !responseFields.count("rawUploadVersion") && !responseFields.count("compactPageVersion"));
 assert(events.front()=="json");
 reset(server);server.identity=false;server.handleStatus();assert(!server._verifiedContact && server.http.code==200 && !memoryReleased);
 assert(!responseFields.count("rawUploadVersion") && !responseFields.count("compactPageVersion")); // middleware rejects wrong target before this method
 reset(server);server.prepareStatusHook=nullptr;server.handleStatus();assert(server.http.code==503 && events.size()==1);
 reset(server);server.handleStatus();events.clear();server.handleStatus();assert(server.http.code==200 && server.http.releasedAt200);
 std::cout<<"PASS: status waits for preparation; errors omit ready response; U4 capabilities retained\n";
#endif
}
