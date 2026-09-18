"""Add bounded physical/USB cancellation while multipart reception is idle.

This patch changes only the pinned WebServer source in the selected private
framework. It runs after the upload/raw patches. No network framing changes.
"""
from pathlib import Path

OLD = '''      while (!timedOut && !client.available() && client.connected()) {
        delay(2);'''
NEW = '''      while (!timedOut && !client.available() && client.connected()) {
#if defined(FLOWE_SYNC_CONTROL)
        if (transfer_sync::pollControls()) { client.stop(); return -1; }
#endif
        delay(2);'''
DECL = '''#if defined(FLOWE_SYNC_CONTROL)
namespace transfer_sync { bool pollControls(); }
#endif
'''

def patched_text(source):
    if NEW in source:
        if source.count(NEW) != 1 or DECL not in source:
            raise RuntimeError("Incomplete sync control patch")
        return source
    if source.count(OLD) != 1:
        raise RuntimeError("Changed Arduino 3.3.7 multipart wait anchor")
    return DECL + source.replace(OLD, NEW, 1)

def apply(framework):
    import json
    framework = Path(framework)
    if json.loads((framework / "package.json").read_text()).get("version") != "3.3.7":
        raise RuntimeError("Sync control requires Arduino 3.3.7")
    path = framework / "libraries/WebServer/src/Parsing.cpp"
    original = path.read_text()
    updated = patched_text(original)
    if updated != original:
        path.write_text(updated)

if "Import" in globals():
    Import("env")
    apply(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
