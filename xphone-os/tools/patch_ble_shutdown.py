"""Put the NimBLE callback barrier before BLE object destruction (Arduino 3.3.7).

The pinned ESP-IDF NimBLE port waits for host stop, then posts a stop event
and waits for that event to run on the host queue. A disconnect callback can
signal host stop before its remaining server work ends; the second wait is
the required callback barrier. Link count and an extra delay are not barriers.

Source inspected: esp-nimble 039d2d62ed97fed632827fd51294f04068b2ca60,
porting/nimble/src/nimble_port.c (nimble_port_stop/run), and
nimble/host/src/ble_hs_stop.c. A successful stop prevents further host event
dispatch. It does not promise that every link disconnected gracefully or
that the host task has finished its final FreeRTOS task cleanup.

A nonzero stop result has no such guarantee, including BLE_HS_EALREADY.
deinit is void and CompanionBleService deletes the GATT graph after it returns.
Abort on failure: returning or deleting anything would permit use-after-free.
Call deinit from the application task, never from a NimBLE host callback.

Only the known original or patched function is accepted. Package and exact
function hashes fail closed on upgrades or partial/conflicting patches.
Importable for host tests; PlatformIO applies it to its selected framework.
Use a private framework package for bench builds, not the shared SDK copy.
"""
import hashlib
import json
from pathlib import Path

ORIGINAL_SHA256 = "0eea5618481bbd8dfda7b534c68cd6574bd9103678b088e36c67c03c18f7b424"
PATCHED_SHA256 = "a670cb099480f490f0a29c5300be78937e78ed696961215c5f19a39d7cf7da2d"
START = "void BLEDevice::deinit(bool release_memory) {"
END = "\n}\n\nvoid BLEDevice::setCustomGapHandler"
INCLUDE = "#include <iomanip>\n"
PATCHED_INCLUDE = INCLUDE + "#include <stdlib.h>  // flowe: abort on unsafe NimBLE shutdown\n"
STOP = """#if defined(CONFIG_NIMBLE_ENABLED)
  // flowe: keep the complete BLE graph alive through the host callback barrier.
  const int stopResult = nimble_port_stop();
  if (stopResult != 0) {
    log_e("NimBLE host stop failed: %d; aborting before BLE object cleanup", stopResult);
    // deinit is void; returning would let the caller delete the GATT graph.
    abort();
  }
#endif

"""


def patch_deinit(body: str) -> str:
    """Transform one exact pinned function, with no file or SCons access."""
    digest = hashlib.sha256(body.encode()).hexdigest()
    if digest == PATCHED_SHA256:
        return body
    if digest != ORIGINAL_SHA256:
        raise RuntimeError("patch_ble_shutdown: unknown BLEDevice::deinit; re-verify the framework")
    body = body.replace("  // Delete all BLE objects\n", STOP + "  // Delete all BLE objects\n", 1)
    body = body.replace("  nimble_port_stop();\n  nimble_port_deinit();", "  nimble_port_deinit();", 1)
    if hashlib.sha256(body.encode()).hexdigest() != PATCHED_SHA256:
        raise RuntimeError("patch_ble_shutdown: internal patch digest mismatch")
    return body


def patch_framework(root: Path) -> bool:
    """Patch the selected package. Validate everything before writing a file."""
    package = json.loads((root / "package.json").read_text())
    if package.get("name") != "framework-arduinoespressif32" or package.get("version") != "3.3.7":
        raise RuntimeError("patch_ble_shutdown: requires framework-arduinoespressif32 3.3.7")
    target = root / "libraries/BLE/src/BLEDevice.cpp"
    source = target.read_text()
    if source.count(START) != 1 or source.count(END) != 1 or source.count(INCLUDE) != 1:
        raise RuntimeError("patch_ble_shutdown: unknown framework anchors")
    start = source.index(START)
    end = source.index(END, start) + len("\n}\n")
    body = source[start:end]
    patched = patch_deinit(body)
    if patched == body:
        if source.count(PATCHED_INCLUDE) != 1:
            raise RuntimeError("patch_ble_shutdown: partial patch; abort include missing")
        return False
    if PATCHED_INCLUDE in source:
        raise RuntimeError("patch_ble_shutdown: partial patch; unexpected abort include")
    source = source[:start] + patched + source[end:]
    source = source.replace(INCLUDE, PATCHED_INCLUDE, 1)
    target.write_text(source)
    return True


if "Import" in globals():
    Import("env")  # noqa: F821 - supplied by PlatformIO/SCons
    framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))  # noqa: F821
    if patch_framework(framework):
        print(f"patch_ble_shutdown: patched {framework}")
