#include "AppScenes.h"

#include <cstring>

#include "AboutScene.h"
#include "AppScene.h"
#include "BlockScene.h"
#include "FileTransferScene.h"
#include "HomeScene.h"
#include "WifiScene.h"
#include "LauncherScene.h"
#include "NotificationsScene.h"
#include "PrioritiesScene.h"
#include "ReaderScene.h"
#include "SettingsScene.h"
#include "TodayScene.h"
#include "WorkoutScene.h"

unsigned long gBootTotalMs = 0;

// M4.2 last-scene restore: updated by every show*() helper below.
SceneId gCurrentSceneId = SceneId::Launcher;

// M4.2 wake diagnostics — set once by main.cpp boot(); defaults hold until then.
const char* gWakeResetReason = "?";
const char* gWakeRestoreScene = "none";

namespace {
// All scenes are static instances — fixed allocation, zero heap churn.
LauncherScene gLauncher;
AboutScene gAbout;
NotificationsScene gNotifications;
SettingsScene gSettings;
BlockScene gBlock;
PrioritiesScene gPriorities;
TodayScene gToday;
WorkoutScene gWorkout;
ReaderScene gReader;
FileTransferScene gFileTransfer;
WifiScene gWifi;
AppScene gApp;
HomeScene gHome;
}  // namespace

void showLauncher() {
  gCurrentSceneId = SceneId::Launcher;
  SCENES.switchTo(gLauncher);
}

void showAbout() {
  gCurrentSceneId = SceneId::About;
  SCENES.switchTo(gAbout);
}

void showNotifications() {
  gCurrentSceneId = SceneId::Notifications;
  SCENES.switchTo(gNotifications);
}

void showSettings() {
  gCurrentSceneId = SceneId::Settings;
  SCENES.switchTo(gSettings);
}

void showBlock() {
  gCurrentSceneId = SceneId::Block;
  SCENES.switchTo(gBlock);
}

void showBlockDeepWork() {
  gCurrentSceneId = SceneId::Block;
  SCENES.switchTo(gBlock);  // runs onEnter (fresh status request)
  gBlock.startDeepWork();   // then fire block.start(deep_work) immediately
}

void showPriorities() {
  gCurrentSceneId = SceneId::Priorities;
  SCENES.switchTo(gPriorities);
}

void showToday() {
  gCurrentSceneId = SceneId::Today;
  SCENES.switchTo(gToday);
}

void showReader() {
  gCurrentSceneId = SceneId::Reader;
  SCENES.switchTo(gReader);
}

void showWorkout() {
  gCurrentSceneId = SceneId::Workout;
  SCENES.switchTo(gWorkout);
}

void showFileTransfer() {
  gCurrentSceneId = SceneId::FileTransfer;
  SCENES.switchTo(gFileTransfer);
}

void showWifi() {
  gCurrentSceneId = SceneId::Wifi;
  SCENES.switchTo(gWifi);
}

void showFileTransferAutoStart() {
  gCurrentSceneId = SceneId::FileTransfer;
  SCENES.switchTo(gFileTransfer);  // onEnter resets to Idle
  gFileTransfer.autoStart();       // then bring the radio up (STA or hotspot)
}

void fileTransferRestartFromCard(const bool direct) {
  gFileTransfer.restartFromCard(direct);
}

void showFileTransferAutoStartDirect() {
  gCurrentSceneId = SceneId::FileTransfer;
  SCENES.switchTo(gFileTransfer);
  gFileTransfer.autoStartDirect();
}

void showFileTransferAutoStartInPlace(const bool direct) {
  const SceneId from = gCurrentSceneId;
  if (G_GFX) SCENES.composeActive(*G_GFX);  // the framebuffer = what is on glass, before the reader lets go
  const Gfx::Orient orientation = G_GFX ? G_GFX->orientation() : Gfx::Orient::Portrait;
  gCurrentSceneId = SceneId::FileTransfer;
  SCENES.switchTo(gFileTransfer);  // the reader's onExit frees the book; the picture stays
  // ReaderScene::onExit restores portrait for normal scene changes. This
  // scene keeps its picture, so the bar must use that picture's orientation.
  if (G_GFX) G_GFX->setOrientation(orientation);
  gFileTransfer.beginSilent(static_cast<uint32_t>(from));
  if (direct) gFileTransfer.autoStartDirect();
  else gFileTransfer.autoStart();
}

void showSceneByIdQuiet(const SceneId id) {
  if (id == SceneId::Reader) gReader.resumeQuietly();
  showSceneById(id);
}

void stopFileTransferIfActive() {
  if (SCENES.active() == &gFileTransfer) gFileTransfer.stopSession();
}

// boot() restore dispatch. Each show*() re-runs the scene's onEnter, which
// re-requests its companion data (Block/Priorities/Today/Notifications all do),
// so a restored scene refreshes itself. Sub-view state (Settings picker,
// Notifications detail, Block modes/break) resets to the scene default — fine.
void showSceneById(SceneId id) {
  switch (id) {
    case SceneId::Notifications: showNotifications(); break;
    case SceneId::Settings:      showSettings();      break;
    case SceneId::Wifi:          showWifi();          break;
    case SceneId::Block:         showBlock();         break;
    case SceneId::Priorities:    showPriorities();    break;
    case SceneId::Today:         showToday();         break;
    case SceneId::About:         showAbout();         break;
    case SceneId::Reader:        showReader();        break;
    case SceneId::Workout:       showWorkout();       break;
    case SceneId::Home:          showHome();          break;
    // FileTransfer deliberately NOT restored: waking straight into a scene
    // that would show a stale Idle menu (the radio never survives sleep)
    // helps nobody — fall through to the launcher.
    case SceneId::FileTransfer:
    case SceneId::Launcher:
    default:                     showLauncher();      break;
  }
}

const char* sceneName(SceneId id) {
  switch (id) {
    case SceneId::Notifications: return "Notifications";
    case SceneId::Settings:      return "Settings";
    case SceneId::Block:         return "Block";
    case SceneId::Priorities:    return "Priorities";
    case SceneId::Today:         return "Today";
    case SceneId::About:         return "About";
    case SceneId::Reader:        return "Reader";
    case SceneId::Workout:       return "Workout";
    case SceneId::Home:          return "Home";
    case SceneId::FileTransfer:  return "Transfer";
    case SceneId::Launcher:      return "Launcher";
    default:                     return "Launcher";
  }
}

void markLauncherDirtyIfActive() {
  if (SCENES.active() == &gLauncher) gLauncher.markDirty();
}

void markNotificationsDirtyIfActive() {
  if (SCENES.active() == &gNotifications) gNotifications.markDirty();
}

void markBlockDirtyIfActive() {
  if (SCENES.active() == &gBlock) gBlock.markDirty();
}

void markPrioritiesDirtyIfActive() {
  if (SCENES.active() == &gPriorities) gPriorities.markDirty();
}

void markTodayDirtyIfActive() {
  if (SCENES.active() == &gToday) gToday.markDirty();
}

void markWorkoutDirtyIfActive() {
  if (SCENES.active() == &gWorkout) gWorkout.markDirty();
}

void showHome() {
  gCurrentSceneId = SceneId::Home;
  SCENES.switchTo(gHome);
}

void homeDebugDump() { gHome.debugDump(); }
bool homeRenderDormant(Gfx& gfx) { return gHome.renderDormant(gfx); }

// Phase 3 (home.layout card): the phone changed the layout, tiles, or
// widget order. Re-read the config; if the person is sitting on a root
// scene, move them to the (possibly new) root so the change shows now.
void applyHomeConfigLive() {
  gHome.loadConfig();
  gLauncher.loadSlots();
  Scene* active = SCENES.active();
  const bool onRoot = active == &gHome || active == &gLauncher;
  if (onRoot) {
    if (homeLayout() == HomeLayout::Widget) {
      showHome();
      gHome.markDirty();
    } else {
      showLauncher();
      gLauncher.markDirty();
    }
  }
}

void markHomeDirtyIfActive() {
  if (SCENES.active() == &gHome) gHome.markDirty();
}

void appDataArrived(const char* name) {
  if (SCENES.active() == &gApp && gApp.loaded() && !strcmp(gApp.appName(), name)) {
    gApp.open(name);  // re-reads app.json + the fresh data.json
    gApp.markDirty();
  }
}

bool showApp(const char* name) {
  if (!gApp.open(name)) return false;
  // Not persisted for wake restore in Phase 1: SceneId has no App entry
  // yet, so sleeping from an app wakes to the launcher. The restore story
  // lands with the home-screen phase, when the app name gets persisted too.
  gCurrentSceneId = SceneId::Launcher;
  SCENES.switchTo(gApp);
  return true;
}

int launcherSelection() { return gLauncher.selection(); }

void readerWhere(char* out, size_t n) { gReader.debugWhere(out, n); }
void readerShelfDump() { gReader.debugShelfDump(); }
void readerLineCids() { gReader.debugLineCids(); }
void readerAcceptGoto(const char* key, uint32_t cid) { gReader.acceptGoto(key, cid); }
