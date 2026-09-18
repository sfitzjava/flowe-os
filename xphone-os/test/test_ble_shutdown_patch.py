"""Run with Python 3; requires a host C++ compiler. --baseline proves the old race.

The fixture is BLEDevice::deinit from Espressif Arduino 3.3.7, unmodified.
Source: https://github.com/espressif/arduino-esp32/blob/3.3.7/libraries/BLE/src/BLEDevice.cpp
"""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
BASELINE = "--baseline" in sys.argv
if BASELINE:
    sys.argv.remove("--baseline")
else:
    spec = importlib.util.spec_from_file_location(
        "patch_ble_shutdown", HERE.parent / "tools/patch_ble_shutdown.py")
    patcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(patcher)

ORIGINAL = (HERE / "fixtures/ble_deinit_3_3_7.inc").read_text()


class Lifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        body = ORIGINAL if BASELINE else patcher.patch_deinit(ORIGINAL)
        code = (HERE / "ble_shutdown_lifecycle.cpp").read_text().replace(
            "// INSERT_FRAMEWORK_DEINIT", body)
        source = Path(cls.temp.name) / "lifecycle.cpp"
        source.write_text(code)
        cls.executables = {}
        for stack in ("NIMBLE", "BLUEDROID"):
            executable = Path(cls.temp.name) / stack
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread",
                            f"-DCONFIG_{stack}_ENABLED=1", "-DCONFIG_BT_CONTROLLER_ENABLED=1",
                            "-DARDUINO_ARCH_ESP32=1", str(source), "-o", str(executable)], check=True)
            cls.executables[stack] = executable

    def run_case(self, scenario, stack="NIMBLE"):
        result = subprocess.run([str(self.executables[stack]), scenario],
                                capture_output=True, text=True, timeout=10)
        return result, result.stdout.splitlines()

    def test_callback_barrier_before_all_deletions(self):
        for scenario in ("success", "pending", "late"):
            with self.subTest(scenario=scenario):
                result, events = self.run_case(scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                barrier = events.index("host_stop_complete")
                for deletion in ("delete_advertising", "delete_scan", "delete_server",
                                 "delete_client", "map_clear", "application_gatt_delete"):
                    self.assertGreater(events.index(deletion), barrier)
                if scenario != "success":
                    self.assertLess(events.index("callback_exit"), barrier)
                self.assertEqual(events.count("host_stop_enter"), 1)
                self.assertNotIn("memory_release", events)

    def test_stop_failure_never_returns_to_application_cleanup(self):
        for scenario in ("failure", "already_stopped"):
            with self.subTest(scenario=scenario):
                result, events = self.run_case(scenario)
                self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
                self.assertIn("host_stop_failed", events)
                self.assertIn("stop_error_log", events)
                for event in events:
                    self.assertNotIn("delete", event)
                self.assertNotIn("host_deinit", events)
                self.assertNotIn("controller_disable", events)
                self.assertNotIn("caller_returned", events)

    def test_release_modes_and_bluedroid(self):
        for stack in ("NIMBLE", "BLUEDROID"):
            for scenario in ("success", "release", "uninitialized"):
                with self.subTest(stack=stack, scenario=scenario):
                    result, events = self.run_case(scenario, stack)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(events.count("memory_release"), int(scenario == "release"))
                    if scenario == "uninitialized":
                        self.assertEqual(events, ["application_gatt_delete", "caller_returned"])
                    elif stack == "BLUEDROID":
                        self.assertNotIn("host_stop_enter", events)
                        self.assertLess(events.index("delete_server"), events.index("bluedroid_disable"))
                        self.assertLess(events.index("bluedroid_disable"), events.index("bluedroid_deinit"))
                    if scenario == "release":
                        self.assertLess(events.index("controller_deinit"), events.index("memory_release"))


@unittest.skipIf(BASELINE, "patch is not applied for red characterization")
class PatchContract(unittest.TestCase):
    def test_repeat_patch_and_unknown_function(self):
        patched = patcher.patch_deinit(ORIGINAL)
        self.assertEqual(patcher.patch_deinit(patched), patched)
        for source in (ORIGINAL.replace("delete m_pServer;", "// unknown teardown"),
                       patched.replace("abort();", "return;"),
                       patched + "// unexpected content\n"):
            with self.assertRaises(RuntimeError):
                patcher.patch_deinit(source)

    def test_private_framework_idempotence_and_version_guard(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            target = root / "libraries/BLE/src/BLEDevice.cpp"
            target.parent.mkdir(parents=True)
            package = root / "package.json"
            package.write_text(json.dumps({"name": "framework-arduinoespressif32", "version": "3.3.7"}))
            original = '#include <iomanip>\n\n' + ORIGINAL + '\nvoid BLEDevice::setCustomGapHandler() {}\n'
            target.write_text(original)
            self.assertTrue(patcher.patch_framework(root))
            patched = target.read_text()
            self.assertFalse(patcher.patch_framework(root))
            self.assertEqual(target.read_text(), patched)
            package.write_text(json.dumps({"name": "framework-arduinoespressif32", "version": "3.3.8"}))
            with self.assertRaises(RuntimeError):
                patcher.patch_framework(root)
            self.assertEqual(target.read_text(), patched)
            package.write_text(json.dumps({"name": "framework-arduinoespressif32", "version": "3.3.7"}))
            target.write_text(original.replace("#include <iomanip>", "// missing include anchor"))
            with self.assertRaises(RuntimeError):
                patcher.patch_framework(root)


if __name__ == "__main__":
    unittest.main()
