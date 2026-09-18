"""Test the production range planner and bounded file hash/read helpers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DownloadRangeTest(unittest.TestCase):
    def test_range_and_stream_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "download-range"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g", "-I", str(ROOT / "src/net"),
                            str(ROOT / "test/host/download_range_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
