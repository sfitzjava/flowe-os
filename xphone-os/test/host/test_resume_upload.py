"""Compile the production resume state machine with fault-injected storage."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ResumeUploadTest(unittest.TestCase):
    def test_production_state_machine(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "resume-upload"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined", "-I", str(ROOT / "src/net"),
                str(ROOT / "test/host/resume_upload_test.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
