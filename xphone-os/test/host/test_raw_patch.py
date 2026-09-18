"""Run with FLOWE_TEST_ARDUINO pointing to the pinned framework (read only)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "tools"
spec = importlib.util.spec_from_file_location("raw_patch", TOOLS / "patch_webserver_raw.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class RawPatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.framework = Path(self.temp.name)
        source = Path(os.environ.get("FLOWE_TEST_ARDUINO", str(Path.home() / ".platformio/packages/framework-arduinoespressif32")))
        self.code = self.framework / "libraries/WebServer/src"
        self.code.mkdir(parents=True)
        for name in patch.REPLACEMENTS:
            shutil.copyfile(source / "libraries/WebServer/src" / name, self.code / name)
        (self.framework / "package.json").write_text(json.dumps({"version": "3.3.7"}))

    def tearDown(self):
        self.temp.cleanup()

    def test_repeat_and_framing_guards(self):
        patch.apply(self.framework)
        before = {p.name: p.read_bytes() for p in self.code.iterdir()}
        patch.apply(self.framework)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.code.iterdir()})
        parser = (self.code / "Parsing.cpp").read_text()
        # Check the dispatch and framing glue surrounding the compiled helper.
        self.assertIn('(_currentUri == "/upload/raw" || !isForm)', parser)
        self.assertIn('if (rawLengthSeen || !flowe_raw::parseLength(', parser)
        self.assertIn('rawLength == 0 || rawEncodingSeen', parser)
        self.assertLess(parser.index('Invalid raw Content-Length'), parser.index('RAW_START'))
        self.assertIn('MSG_PEEK | MSG_DONTWAIT', parser)
        self.assertIn('searchStr.length() > 768', parser)

    def test_unknown_anchor_and_missing_helper_fail_before_any_write(self):
        original = {p.name: p.read_bytes() for p in self.code.iterdir()}
        with self.assertRaises(FileNotFoundError):
            patch.apply(self.framework, self.framework / "missing.h")
        self.assertEqual(original, {p.name: p.read_bytes() for p in self.code.iterdir()})
        cpp = self.code / "WebServer.cpp"
        cpp.write_text(cpp.read_text().replace('_currentRaw.reset();', '_currentRaw.release();'))
        before = {p.name: p.read_bytes() for p in self.code.iterdir()}
        with self.assertRaises(RuntimeError):
            patch.apply(self.framework)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.code.iterdir()})

    def test_version_pin(self):
        (self.framework / "package.json").write_text('{"version":"3.3.8"}')
        with self.assertRaises(RuntimeError):
            patch.apply(self.framework)

    def test_actual_sd_write_and_close_methods(self):
        source = (TOOLS.parent / "src/net/FileTransferServer.cpp").read_text()
        def method(start, end):
            return source[source.index(start):source.index(end, source.index(start))]
        close = method("void FileTransferServer::closeUploadFile() {", "\n#if defined(FLOWE_BENCH_UPLOAD_PROFILE)\nvoid FileTransferServer::reportUploadProfile")
        flush = method("bool FileTransferServer::flushUploadBuffer() {", "\n\n#if defined(FLOWE_RAW_UPLOAD)")
        harness = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
struct File {
  size_t writeResult = 0, writes = 0; bool closeResult = true; uint8_t bytes[4096]{};
  explicit operator bool() { return true; }
  size_t write(const uint8_t* data, size_t size) { ++writes; memcpy(bytes,data,size); return writeResult; }
  bool close() { return closeResult; }
} gUploadFile;
void esp_task_wdt_reset() {}
struct FileTransferServer {
  struct State { size_t bufferPos=0; bool failed=false; uint8_t buffer[4096]{}; } _upload;
  uint8_t* uploadWriteBuffer() { return _upload.buffer; }
  bool flushUploadBuffer(); void closeUploadFile();
};
""" + close + flush + r"""
int main() {
  FileTransferServer server;
  for(size_t written: {size_t(0),size_t(4095),size_t(4096)}) {
    server._upload.bufferPos=4096; gUploadFile.writeResult=written;
    assert(server.flushUploadBuffer() == (written==4096));
    assert(server._upload.bufferPos==0);
  }
  server._upload.bufferPos=19; gUploadFile.writeResult=19;
  for(size_t i=0;i<19;++i)server._upload.buffer[i]=i;
  assert(server.flushUploadBuffer());assert(!memcmp(server._upload.buffer,gUploadFile.bytes,19));
  server._upload.failed=false;gUploadFile.closeResult=false;server.closeUploadFile();assert(server._upload.failed);
  server._upload.failed=false;gUploadFile.closeResult=true;server.closeUploadFile();assert(!server._upload.failed);
}
"""
        program = self.framework / "sd.cpp"
        program.write_text("#include <initializer_list>\n" + harness)
        binary = self.framework / "sd-test"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        str(program), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_compiled_receive_and_publication(self):
        binary = self.framework / "raw-test"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        str(Path(__file__).with_name("raw_upload_test.cpp")), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
