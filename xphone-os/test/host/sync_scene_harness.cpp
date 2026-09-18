// Test doubles for the exact FileTransferScene methods inserted by the runner.
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
std::vector<std::string> events;
bool sSyncDisplay=false,sSilentNow=false,gWifiTestMode=false;
bool cancelFlag=false,httpActive=false;int controlPolls=0,waitCount=0,dropRouteAt=0,cancelAt=0;
uint32_t nowMs=0;uint32_t millis(){return nowMs;}
namespace transfer_sync {
bool handlingHttp(){return httpActive;}
bool cancelRequested(){return cancelFlag;}
bool pollControls(){++controlPolls;return cancelFlag;}
}
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,TCP_WND=5760,TCP_SND_BUF=5744,WL_CONNECTED=1;
unsigned heap_caps_get_largest_free_block(int){return 60000;}
struct {unsigned getFreeHeap(){return 60000;}} ESP;
struct {template<class...A>void printf(const char*,A...){} void println(const char*){}} Serial;
struct {int clients=1,sta=1;int softAPgetStationNum(){return clients;}int status(){return sta;}} WiFi;
struct Gfx {
 enum class Orient {Portrait,Landscape}; Orient orient=Orient::Portrait; bool releaseOk=true,restoreOk=true,released=false;
 void setOrientation(Orient value){orient=value;}
 unsigned getBufferSize(){return 48000;} Gfx& display(){return *this;}
 bool releaseFramebufferForSync(){events.push_back("release");released=releaseOk;return releaseOk;}
 bool restoreFramebufferAfterSync(){events.push_back("restore");if(restoreOk)released=false;return restoreOk;}
} gfx;
Gfx* G_GFX=&gfx;
struct FileTransferScene;
struct Scenes {
 bool pause=false,renderOk=true;FileTransferScene* scene=nullptr;
 bool paused(){return pause;}void setPaused(bool p){pause=p;}
 void waitFlushIdle(){
  events.push_back("wait");++waitCount;
  if(waitCount==dropRouteAt){WiFi.sta=0;WiFi.clients=0;}
  if(waitCount==cancelAt)cancelFlag=true;
 }
 void renderIfDirty(Gfx&);
} SCENES;
enum class SceneId:uint32_t {Launcher=0,Reader=9};
struct Restart {uint32_t scene;};
void quietRestartToScene(uint32_t n){events.push_back("restart");throw Restart{n};}
void transferSetSessionToken(const char*, bool){}
struct Preferences {bool begin(const char*,bool){return true;}void putUChar(const char*,int){}void end(){}};
struct {void end(){events.push_back("mdns-stop");}} MDNS;
struct {void resumeAfterTransfer(const char*,const char*){events.push_back("ble-resume");}void sendTransferStatus(const char*,const char*,const char*){}} COMPANION_BLE;
void stackProbe(const char*){}void transferMemoryProbe(const char*){}
void showWifi(){events.push_back("wifi-scene");}void showLauncher(){events.push_back("launcher");}
void showSceneByIdQuiet(SceneId){events.push_back("return-scene");}
struct FileTransferScene {
 enum class State{Idle,Connecting,Running,Failed};State _state=State::Running;
 bool _staticSync=false,_framebufferReleased=false,_silent=true,_inPlace=true,_directMode=false,_radioWasUp=true,dirty=false;
 bool paintOk=true;char _pillShown[40]={};
 uint32_t _returnSceneId=9,_failedAtMs=0,_silentSinceMs=0;
 struct Server {bool contact=true;void* progressHook=nullptr;bool verifiedContact(){return contact;}void stop(){events.push_back("server-stop");}} _server;
 void markDirty(){dirty=true;}void clearDirty(){dirty=false;}bool isDirty(){return dirty;}
 bool paintPill(const char* text){
  if(!paintOk || _staticSync || gfx.released)return false;
  events.push_back("bar");snprintf(_pillShown,sizeof(_pillShown),"%s",text);return true;
 }
 void teardownRadio(){events.push_back("radio-off");_radioWasUp=false;}
 bool activateSyncMemory(bool associatedHotspot=false);void restoreSyncMemory();void onExit();void silentTick();
 void endSession(const char*,bool=true,bool=false);
};
void Scenes::renderIfDirty(Gfx&){events.push_back("render");if(renderOk)scene->dirty=false;}
// PRODUCTION_METHODS
static void reset(FileTransferScene& s){s=FileTransferScene{};SCENES=Scenes{};SCENES.scene=&s;gfx=Gfx{};G_GFX=&gfx;WiFi.clients=WiFi.sta=1;sSyncDisplay=false;gWifiTestMode=false;cancelFlag=false;httpActive=false;controlPolls=waitCount=dropRouteAt=cancelAt=nowMs=0;events.clear();}
int main(){
 FileTransferScene s;
 // Regression: phone sync retains its picture and orientation when RAM is released.
 reset(s);gfx.orient=Gfx::Orient::Landscape;
 assert(s.activateSyncMemory());
 assert(s._silent && gfx.orient==Gfx::Orient::Landscape);
 assert(std::find(events.begin(),events.end(),"render")==events.end());
 // Route/cancel changes during either flush must keep ownership and omit readiness.
 for(bool direct:{false,true})for(bool silent:{false,true})for(int at=1;at<=(silent?2:3);++at){
  reset(s);s._directMode=direct;s._silent=silent;dropRouteAt=at;httpActive=true;
  assert(!s.activateSyncMemory());assert(!s._framebufferReleased && !SCENES.pause && !sSyncDisplay);
  assert(events.back()=="wait" && controlPolls>=2);
  WiFi.sta=WiFi.clients=1;dropRouteAt=0;assert(s.activateSyncMemory());
  reset(s);s._directMode=direct;s._silent=silent;cancelAt=at;httpActive=true;assert(!s.activateSyncMemory());
  assert(!s._framebufferReleased && !SCENES.pause && cancelFlag);
 }
 reset(s);cancelFlag=true;assert(!s.activateSyncMemory() && events.empty());
 reset(s);assert(s.activateSyncMemory());assert(controlPolls==0); // no recursive console from outer tick
 reset(s);s._server.contact=false;s.activateSyncMemory();assert(events.empty());
 // Association prepares only AP RAM, and does not claim verified contact.
 reset(s);s._server.contact=false;s._directMode=true;
 assert(s.activateSyncMemory(true));assert(s._framebufferReleased && !s._server.contact);
 reset(s);s._server.contact=false;s._directMode=false;
 assert(!s.activateSyncMemory(true) && events.empty());
 reset(s);s._server.contact=false;s._directMode=true;WiFi.clients=0;
 assert(!s.activateSyncMemory(true) && events.empty());
 for(int at:{1,2}){
  reset(s);s._server.contact=false;s._directMode=true;dropRouteAt=at;
  assert(!s.activateSyncMemory(true) && !s._framebufferReleased && !SCENES.pause);
  reset(s);s._server.contact=false;s._directMode=true;cancelAt=at;
  assert(!s.activateSyncMemory(true) && !s._framebufferReleased && !SCENES.pause);
 }
 reset(s);WiFi.sta=0;s.activateSyncMemory();assert(events.empty());
 reset(s);s._directMode=true;WiFi.clients=0;s.activateSyncMemory();assert(events.empty());
 reset(s);gWifiTestMode=true;s.activateSyncMemory();assert(events.empty());
 reset(s);s._silent=false;SCENES.renderOk=false;s.activateSyncMemory();assert(!s._framebufferReleased && !SCENES.pause);
 reset(s);s.paintOk=false;assert(!s.activateSyncMemory() && !s._framebufferReleased && !SCENES.pause);
 for(bool direct:{false,true})for(bool inPlace:{false,true}){
  reset(s);s._directMode=direct;s._inPlace=s._silent=inPlace;s.activateSyncMemory();
  const std::vector<std::string> expected=inPlace?
   std::vector<std::string>{"wait","bar","wait","release"}:
   std::vector<std::string>{"wait","render","wait","bar","wait","release"};
  assert(events==expected);
  assert(s._staticSync && s._silent==inPlace && s._framebufferReleased && SCENES.pause && sSyncDisplay);
  s.activateSyncMemory();assert(events==expected);
  events.clear();try{s.endSession("normal");assert(false);}catch(Restart r){assert(r.scene==(inPlace?9u:0u));}
  assert((events==std::vector<std::string>{"wait","server-stop","mdns-stop","radio-off","restart"}));
 }
 reset(s);s.activateSyncMemory();events.clear();s.endSession("test",false,true);
 assert((events==std::vector<std::string>{"wait","server-stop","mdns-stop","radio-off","restore","ble-resume","wifi-scene"}));
 assert(!sSyncDisplay && !SCENES.pause && !gfx.released);
 assert(gfx.orient==Gfx::Orient::Portrait);
 reset(s);s.activateSyncMemory();gfx.restoreOk=false;
 try{s.endSession("test",false,true);assert(false);}catch(Restart r){assert(r.scene==9);}
 reset(s);s.activateSyncMemory();events.clear();try{s.onExit();assert(false);}catch(Restart r){assert(r.scene==9);}
 assert(events.back()=="restart");
 reset(s);gfx.releaseOk=false;s.activateSyncMemory();assert(s._staticSync && !s._framebufferReleased && SCENES.pause);
 // Waiting and failure update only the bar. Neither reveals another page.
 reset(s);s._directMode=true;WiFi.clients=0;nowMs=60000;s.silentTick();
 assert(s._silent && std::string(s._pillShown)=="Waiting for your phone...");
 assert((events==std::vector<std::string>{"bar"}));
 events.clear();s._state=FileTransferScene::State::Failed;s.silentTick();
 assert(s._silent && std::string(s._pillShown)=="Sync failed");
 assert((events==std::vector<std::string>{"bar"}));
 // After RAM release, losing the phone cannot repaint or reveal the QR page.
 reset(s);s._directMode=true;s.activateSyncMemory();events.clear();WiFi.clients=0;nowMs=60000;s.silentTick();
 assert(s._silent && events.empty());
 std::cout<<"PASS: exact scene methods retain the page and cover contact, STA/AP, flush, cancel, fallback, exits, restore OOM\n";
}
