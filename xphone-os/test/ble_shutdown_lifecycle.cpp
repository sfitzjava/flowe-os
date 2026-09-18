// The Python driver inserts the exact framework deinit below these stubs.
// No timing sleeps: a condition variable models a pending host callback.
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

static bool stopped = false;
static bool callbackActive = false;
static std::string scenario;
static std::mutex gate;
static std::condition_variable changed;
static bool callbackEntered = false, finishCallback = false;
static std::thread host;
static void event(const char* text) { std::puts(text); }
static void require(bool value) {
  if (!value) throw std::runtime_error("object deletion before host callback barrier");
}
struct Object {
  const char* name;
  bool alive = true;
  void stop() { require(alive); }
  ~Object() {
#ifdef CONFIG_NIMBLE_ENABLED
    require(stopped && !callbackActive);
#endif
    alive = false;
    event(name);
  }
};
struct PeerMap { void clear() { event("map_clear"); } };
class BLEDevice {
 public:
  static bool initialized, m_synced;
  static Object *m_bleAdvertising, *m_pScan, *m_pServer, *m_pClient;
  static PeerMap m_connectedClientsMap;
  static void deinit(bool release_memory);
};
bool BLEDevice::initialized = true, BLEDevice::m_synced = true;
Object* BLEDevice::m_bleAdvertising = new Object{"delete_advertising"};
Object* BLEDevice::m_pScan = new Object{"delete_scan"};
Object* BLEDevice::m_pServer = new Object{"delete_server"};
Object* BLEDevice::m_pClient = new Object{"delete_client"};
PeerMap BLEDevice::m_connectedClientsMap;

static void callback() {
  callbackActive = true;
  event("callback_enter");
  for (auto object : {BLEDevice::m_bleAdvertising, BLEDevice::m_pScan,
                      BLEDevice::m_pServer, BLEDevice::m_pClient})
    require(object != nullptr && object->alive);
  if (scenario == "pending") {
    std::unique_lock<std::mutex> lock(gate);
    callbackEntered = true;
    changed.notify_all();
    changed.wait(lock, [] { return finishCallback; });
  }
  event("callback_exit");
  callbackActive = false;
}
static int nimble_port_stop() {
  event("host_stop_enter");
  if (scenario == "failure" || scenario == "already_stopped") {
    event("host_stop_failed");
    return scenario == "failure" ? 12 : 2;
  }
  if (scenario == "late") callback();  // Link arrives after shutdown starts.
  if (scenario == "pending") {
    { std::lock_guard<std::mutex> lock(gate); finishCallback = true; }
    changed.notify_all();
    host.join();
  }
  stopped = true;
  event("host_stop_complete");
  return 0;
}
static void nimble_port_deinit() { require(stopped); event("host_deinit"); }
static void esp_bluedroid_disable() { event("bluedroid_disable"); }
static void esp_bluedroid_deinit() { event("bluedroid_deinit"); }
static void esp_bt_controller_disable() { event("controller_disable"); }
static void esp_bt_controller_deinit() { event("controller_deinit"); }
static void esp_bt_controller_mem_release(int) { event("memory_release"); }
#define ESP_BT_MODE_BTDM 0
#define log_e(...) event("stop_error_log")

// INSERT_FRAMEWORK_DEINIT

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  scenario = argc > 1 ? argv[1] : "success";
  if (scenario == "uninitialized") BLEDevice::initialized = false;
  if (scenario == "pending") {
    host = std::thread(callback);
    std::unique_lock<std::mutex> lock(gate);
    changed.wait(lock, [] { return callbackEntered; });
  }
  BLEDevice::deinit(scenario == "release");
  // This is the application's behavior after the void deinit call.
  event("application_gatt_delete");
  if (scenario != "uninitialized") {
    require(!BLEDevice::initialized);
    require(BLEDevice::m_pServer == nullptr && BLEDevice::m_pClient == nullptr);
#ifdef CONFIG_NIMBLE_ENABLED
    require(!BLEDevice::m_synced);
#else
    require(BLEDevice::m_synced);
#endif
    BLEDevice::deinit(false);  // Repeated deinit must remain inert.
  }
  event("caller_returned");
}
