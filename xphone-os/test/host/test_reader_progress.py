"""Check the real history sender under the allocation failure seen on X4."""
from pathlib import Path
import datetime
import json
import subprocess
import tempfile
import unittest

OS = Path(__file__).resolve().parents[2]


class ReaderProgressTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "progress"
        source = (OS / "src/ble/CompanionBleService.cpp").read_text()
        start = source.index("void CompanionBleService::sendReaderProgress()")
        end = source.index("\nvoid CompanionBleService::sendReaderShelf()", start)
        harness = (OS / "test/host/reader_progress_test.cpp").read_text()
        test = Path(cls.tmp.name) / "progress.cpp"
        test.write_text(harness.replace("// PRODUCTION_METHOD", source[start:end]))
        subprocess.run(["c++", "-std=c++17", "-g", "-fsanitize=address,undefined",
                        "-DCONFIG_NIMBLE_ENABLED=1", "-I" + str(OS / "test/host/stats_stubs"),
                        "-I" + str(OS / "src"), str(test), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_sender(self, mtu, limit, disconnect=0):
        p = subprocess.run([str(self.exe), str(mtu), str(limit), str(disconnect)], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        frames = [json.loads(line) for line in p.stdout.splitlines()]
        for raw, frame in zip(p.stdout.splitlines(), frames):
            self.assertLessEqual(len(raw.encode()), min(mtu - 3, 512))
            self.assertEqual(frame["type"], "reader.progress")
            self.assertEqual(frame["schemaVersion"], 1)
        return frames, p.stderr

    def test_full_history_survives_lack_of_large_blocks(self):
        for mtu in [127, 185, 517, 1000]:
            with self.subTest(mtu=mtu):
                frames, _ = self.run_sender(mtu, 8192)
                self.assertEqual([x["seq"] for x in frames], list(range(len(frames))))
                self.assertEqual(len({x["tok"] for x in frames}), 1)
                self.assertTrue(frames[-1]["done"])
                self.assertTrue(all(not x["done"] for x in frames[:-1]))
                payload = "".join(x["d"] for x in frames)
                self.assertTrue(all(x["len"] == len(payload.encode()) for x in frames))
                obj = json.loads(payload)
                days = [{"day": int((datetime.date(2026, 1, 1) + datetime.timedelta(days=i)).strftime("%Y%m%d")),
                         "pages": 65535, "minutes": 65535} for i in range(64)]
                books = [{"hash": 2**32 - 1 - i, "pages": 2**32 - 1, "minutes": 2**32 - 1,
                          "lastDay": 20261231, "firstDay": 20260101, "daysRead": 65535} for i in range(64)]
                self.assertEqual(obj, {"clockValid": True, "streak": 0, "todayPages": 0, "days": days, "books": books})

    def test_serialization_needs_no_cpp_heap(self):
        _, report = self.run_sender(517, 0)
        self.assertIn("serialization_allocations=0", report)

    def test_mtu_not_ready_defers_before_building_history(self):
        frames, report = self.run_sender(23, 0)
        self.assertFalse(frames)
        self.assertIn("retry=1", report)

    def test_disconnect_stops_without_a_false_completion(self):
        frames, _ = self.run_sender(185, 0, 3)
        self.assertEqual(len(frames), 3)
        self.assertTrue(all(not x["done"] for x in frames))


if __name__ == "__main__":
    unittest.main()
