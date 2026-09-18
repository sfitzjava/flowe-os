"""Keep multipart upload storage reserved for Flowe's pinned WebServer build.

The X3 hotspot crashed in Parsing.cpp's new HTTPUpload after several shelf
and position requests fragmented the heap. The application reserves that
storage at server startup. Reuse it across requests, and fail a form cleanly
if its smaller argument allocation cannot fit. Other builds retain upstream
behavior unless FLOWE_WEBSERVER_RESERVED_UPLOAD is set.
"""
from pathlib import Path

Import("env")  # noqa: F821

source = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32")) / "libraries/WebServer/src"  # noqa: F821
marker = "FLOWE_WEBSERVER_RESERVED_UPLOAD"

def patch(path, replacements):
    text = path.read_text()
    if marker in text:
        if any(new not in text for _, new in replacements):
            raise RuntimeError(f"patch_webserver_upload: incomplete or changed patch in {path}")
        return
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError(f"patch_webserver_upload: changed framework anchor in {path}: {old}")
        text = text.replace(old, new, 1)
    path.write_text(text)

patch(source / "Parsing.cpp", [
    ("    _postArgs = new RequestArgument[WEBSERVER_MAX_POST_ARGS];", """#if defined(FLOWE_WEBSERVER_RESERVED_UPLOAD)
    _postArgs = new (std::nothrow) RequestArgument[WEBSERVER_MAX_POST_ARGS];
    if (!_postArgs) { send(503, "text/plain", "Reader busy; retry"); return false; }
#else
    _postArgs = new RequestArgument[WEBSERVER_MAX_POST_ARGS];
#endif"""),
    ("            _currentUpload.reset(new HTTPUpload());", """#if defined(FLOWE_WEBSERVER_RESERVED_UPLOAD)
            // Reserved by FileTransferServer before request traffic fragments heap.
            if (!_currentUpload) { send(503, "text/plain", "Reader busy; retry"); return false; }
#else
            _currentUpload.reset(new HTTPUpload());
#endif"""),
    ("    _currentArgs = new RequestArgument[_postArgsLen];", """#if defined(FLOWE_WEBSERVER_RESERVED_UPLOAD)
    _currentArgs = new (std::nothrow) RequestArgument[_postArgsLen];
    if (!_currentArgs && _postArgsLen) { send(503, "text/plain", "Reader busy; retry"); return false; }
#else
    _currentArgs = new RequestArgument[_postArgsLen];
#endif"""),
])

patch(source / "WebServer.cpp", [
    ("    _currentUpload.reset();", """#if defined(FLOWE_WEBSERVER_RESERVED_UPLOAD)
    if (_currentUpload) {
      _currentUpload->name = String();
      _currentUpload->filename = String();
      _currentUpload->type = String();
    }
#else
    _currentUpload.reset();
#endif"""),
])
