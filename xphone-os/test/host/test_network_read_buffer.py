"""Pinned NetworkClient growth patch and byte-ownership regression.

FLOWE_TEST_ARDUINO selects a read-only Arduino 3.3.7 package. Every patch runs
on a temporary copy. FLOWE_TEST_BUFFER_PATCH is only needed before integration
or to test a different patch generator explicitly.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[1] / "tools"
PATCH_PATH = Path(os.environ.get("FLOWE_TEST_BUFFER_PATCH", str(TOOLS / "patch_network_read_buffer.py")))
spec = importlib.util.spec_from_file_location("network_read_buffer_patch", PATCH_PATH)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


def function(source, signature):
    """Extract a known simple C++ method without copying its implementation."""
    start = source.index(signature)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class NetworkReadBufferPatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.framework = Path(self.temp.name)
        self.code = self.framework / "libraries/Network/src"
        self.code.mkdir(parents=True)
        package = Path(os.environ.get("FLOWE_TEST_ARDUINO", str(Path.home() / ".platformio/packages/framework-arduinoespressif32")))
        shutil.copyfile(package / "package.json", self.framework / "package.json")
        self.assertEqual(json.loads((self.framework / "package.json").read_text()).get("version"), "3.3.7", "Test fixture must be the pinned framework")
        for name, replacements in patch.REPLACEMENTS.items():
            text = (package / "libraries/Network/src" / name).read_text()
            # A developer package can already have this exact patch applied.
            # Normalize only complete known blocks in our private test copy.
            for old, new in reversed(replacements):
                if text.count(new) == 1:
                    text = text.replace(new, old, 1)
            (self.code / name).write_text(text)

    def snapshot(self):
        return {name: (self.code / name).read_bytes() for name in patch.REPLACEMENTS}

    def test_idempotent(self):
        patch.apply(self.framework)
        before = self.snapshot()
        patch.apply(self.framework)
        self.assertEqual(before, self.snapshot())

    def test_unknown_header_anchor_fails_before_any_write(self):
        # The header is validated after CPP. This proves CPP is not written
        # first when a later input is incompatible.
        header = self.code / "NetworkClient.h"
        old = patch.REPLACEMENTS["NetworkClient.h"][0][0]
        header.write_text(header.read_text().replace(old, "  // unavailable pinned declaration", 1))
        before = self.snapshot()
        with self.assertRaises(RuntimeError):
            patch.apply(self.framework)
        self.assertEqual(before, self.snapshot())

    def test_duplicate_anchor_fails_before_any_write(self):
        cpp = self.code / "NetworkClient.cpp"
        old = patch.REPLACEMENTS["NetworkClient.cpp"][0][0]
        cpp.write_text(cpp.read_text() + "\n" + old)
        before = self.snapshot()
        with self.assertRaises(RuntimeError):
            patch.apply(self.framework)
        self.assertEqual(before, self.snapshot())

    def test_wrong_version_fails_before_any_write(self):
        package = self.framework / "package.json"
        info = json.loads(package.read_text())
        info["version"] = "3.3.8"
        package.write_text(json.dumps(info))
        before = self.snapshot()
        with self.assertRaises(RuntimeError):
            patch.apply(self.framework)
        self.assertEqual(before, self.snapshot())

    def test_drifted_existing_patch_fails_before_any_write(self):
        patch.apply(self.framework)
        cpp = self.code / "NetworkClient.cpp"
        source = cpp.read_text()
        self.assertIn("kMaxRawReadBuffer = 5744", source)
        cpp.write_text(source.replace("kMaxRawReadBuffer = 5744", "kMaxRawReadBuffer = 5745", 1))
        before = self.snapshot()
        with self.assertRaises(RuntimeError):
            patch.apply(self.framework)
        self.assertEqual(before, self.snapshot())

    def test_actual_rx_preserves_bytes_and_ownership(self):
        patch.apply(self.framework)
        cpp = (self.code / "NetworkClient.cpp").read_text()
        header = (self.code / "NetworkClient.h").read_text()
        rx = cpp[cpp.index("class NetworkClientRxBuffer {"):cpp.index("\nclass NetworkClientSocketHandle {")]
        declaration = re.findall(r"^\s*bool\s+floweGrowReadBuffer\s*\([^;]*\);", header, re.M)
        self.assertEqual(len(declaration), 1)
        adapter = r"""
class NetworkClient {
public:
  std::shared_ptr<NetworkClientRxBuffer> _rxBuffer;
  int socketfd; bool _connected = true;
  unsigned _timeout = 10000, _lastReadTimeout = 10000;
  explicit NetworkClient(int fd):_rxBuffer(new NetworkClientRxBuffer(fd)),socketfd(fd){}
  ~NetworkClient(){stop();}
  int fd(){return socketfd;}
  int setSocketOption(int option,char* value,size_t size){return setsockopt(socketfd,SOL_SOCKET,option,value,size);}
  void stop(){if(socketfd>=0)::close(socketfd);socketfd=-1;_rxBuffer.reset();_connected=false;}
  int read(uint8_t*,size_t); int available(); uint8_t connected();
DECLARATION
};
""".replace("DECLARATION", declaration[0])
        methods = "\n".join(function(cpp, signature) for signature in (
            "int NetworkClient::read(uint8_t *buf, size_t size)",
            "int NetworkClient::available()",
            "uint8_t NetworkClient::connected()",
            "bool NetworkClient::floweGrowReadBuffer(size_t size)",
        ))
        (self.framework / "PinnedNetworkClient.inc").write_text(rx + adapter + methods)
        binary = self.framework / "rx-test"
        command = [os.environ.get("CXX", "clang++"), "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-I", str(self.framework), str(HERE / "network_read_buffer_test.cpp"), "-o", str(binary)]
        built = subprocess.run(command, text=True, capture_output=True, timeout=30)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn("PASS:", ran.stdout)


if __name__ == "__main__":
    unittest.main()
