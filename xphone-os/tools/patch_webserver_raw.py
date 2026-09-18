"""Pinned Arduino 3.3.7 raw upload patch. Importing this module has no side effects.

Exact before/after anchors reject unknown or partial core changes. The existing
multipart reservation patch must run first. No general NetworkClient change.
"""
from pathlib import Path

REPLACEMENTS = {'Parsing.cpp': [('#include "NetworkClient.h"\n'
                  '#include "WebServer.h"\n'
                  '#include "detail/mimetable.h"\n'
                  '\n'
                  '#ifndef WEBSERVER_MAX_POST_ARGS\n'
                  '#define WEBSERVER_MAX_POST_ARGS 32\n',
                  '#include "NetworkClient.h"\n'
                  '#include "WebServer.h"\n'
                  '#include "detail/mimetable.h"\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '#include <errno.h>\n'
                  '#include <lwip/sockets.h>\n'
                  '#include "FloweRawReceive.h"\n'
                  '#endif\n'
                  '\n'
                  '#ifndef WEBSERVER_MAX_POST_ARGS\n'
                  '#define WEBSERVER_MAX_POST_ARGS 32\n'),
                 ('    String headerValue;\n'
                  '    bool isForm = false;\n'
                  '    bool isEncoded = false;\n'
                  '    //parse headers\n'
                  '    while (1) {\n'
                  "      req = client.readStringUntil('\\r');\n",
                  '    String headerValue;\n'
                  '    bool isForm = false;\n'
                  '    bool isEncoded = false;\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  "    // Validate the raw length separately from String::toInt's permissive parse.\n"
                  '    bool rawLengthSeen = false, rawLengthValid = true, rawEncodingSeen = false;\n'
                  '    uint32_t rawLength = 0;\n'
                  '#endif\n'
                  '    //parse headers\n'
                  '    while (1) {\n'
                  "      req = client.readStringUntil('\\r');\n"),
                 ('      headerValue.trim();\n'
                  '      _collectHeader(headerName.c_str(), headerValue.c_str());\n'
                  '\n'
                  '      if (headerName.equalsIgnoreCase(FPSTR(Content_Type))) {\n'
                  '        using namespace mime;\n'
                  '        if (headerValue.startsWith(FPSTR(mimeTable[txt].mimeType))) {\n',
                  '      headerValue.trim();\n'
                  '      _collectHeader(headerName.c_str(), headerValue.c_str());\n'
                  '\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '      if (headerName.equalsIgnoreCase(F("Transfer-Encoding"))) rawEncodingSeen = true;\n'
                  '      if (headerName.equalsIgnoreCase(F("Content-Length"))) {\n'
                  '        if (rawLengthSeen || !flowe_raw::parseLength(headerValue.c_str(), '
                  'headerValue.length(), rawLength))\n'
                  '          rawLengthValid = false;\n'
                  '        rawLengthSeen = true;\n'
                  '      }\n'
                  '#endif\n'
                  '      if (headerName.equalsIgnoreCase(FPSTR(Content_Type))) {\n'
                  '        using namespace mime;\n'
                  '        if (headerValue.startsWith(FPSTR(mimeTable[txt].mimeType))) {\n'),
                 ('      }\n'
                  '    }\n'
                  '\n'
                  '    if (!isForm && _currentHandler && _currentHandler->canRaw(*this, _currentUri)) {\n'
                  '      log_v("Parse raw");\n'
                  '      _currentRaw.reset(new HTTPRaw());\n'
                  '      _currentRaw->status = RAW_START;\n'
                  '      _currentRaw->totalSize = 0;\n'
                  '      _currentRaw->currentSize = 0;\n',
                  '      }\n'
                  '    }\n'
                  '\n'
                  '    if (_currentHandler && _currentHandler->canRaw(*this, _currentUri) &&\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '        (_currentUri == "/upload/raw" || !isForm)) {\n'
                  '#else\n'
                  '        !isForm) {\n'
                  '#endif\n'
                  '      log_v("Parse raw");\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '      // Stay in this raw branch on rejection: false canRaw would enter the\n'
                  '      // whole-body allocator. No callback/file creation occurs before checks.\n'
                  '      if (!rawLengthSeen || !rawLengthValid || rawLength == 0 || rawEncodingSeen) {\n'
                  '        send(400, "text/plain", "Invalid raw Content-Length or Transfer-Encoding");\n'
                  '        return false;\n'
                  '      }\n'
                  '      if (!_currentRaw) {\n'
                  '        send(503, "text/plain", "Reader busy; raw storage unavailable");\n'
                  '        return false;\n'
                  '      }\n'
                  '      if (searchStr.length() > 768) {\n'
                  '        send(400, "text/plain", "Raw query too long");\n'
                  '        return false;\n'
                  '      }\n'
                  '      unsigned rawArgs = 1;\n'
                  "      for (size_t i = 0; i < searchStr.length(); ++i) if (searchStr[i] == '&') "
                  '++rawArgs;\n'
                  '      if (rawArgs > 4) {\n'
                  '        send(400, "text/plain", "Too many raw query arguments");\n'
                  '        return false;\n'
                  '      }\n'
                  '      _parseArguments(searchStr);\n'
                  '      if (!_currentArgs) {\n'
                  '        send(503, "text/plain", "Reader busy; raw arguments unavailable");\n'
                  '        return false;\n'
                  '      }\n'
                  '      _clientContentLength = static_cast<int>(rawLength);\n'
                  '      _currentRaw->data = nullptr;\n'
                  '#else\n'
                  '      _currentRaw.reset(new HTTPRaw());\n'
                  '#endif\n'
                  '      _currentRaw->status = RAW_START;\n'
                  '      _currentRaw->totalSize = 0;\n'
                  '      _currentRaw->currentSize = 0;\n'),
                 ('      _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '      _currentRaw->status = RAW_WRITE;\n'
                  '\n'
                  '      while (_currentRaw->totalSize < (size_t)_clientContentLength) {\n'
                  '        size_t read_len = std::min((size_t)_clientContentLength - _currentRaw->totalSize, '
                  '(size_t)HTTP_RAW_BUFLEN);\n'
                  '        _currentRaw->currentSize = client.readBytes(_currentRaw->buf, read_len);\n',
                  '      _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '      _currentRaw->status = RAW_WRITE;\n'
                  '\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '      // Preserve full callback chunks, but do not wait another full timeout\n'
                  '      // after FIN or a partial timed-out chunk. All reads use the reserved\n'
                  '      // raw buffer. Five seconds with no received bytes remains the limit.\n'
                  '      while (_currentRaw->totalSize < static_cast<size_t>(_clientContentLength)) {\n'
                  '        const size_t wanted = std::min(static_cast<size_t>(_clientContentLength) - '
                  '_currentRaw->totalSize,\n'
                  '                                       static_cast<size_t>(HTTP_RAW_BUFLEN));\n'
                  '        const auto chunk = flowe_raw::receive(client, _currentRaw->buf, wanted,\n'
                  '            [] { return millis(); }, [](unsigned ms) { delay(ms); },\n'
                  '            [](int fd) {\n'
                  '              uint8_t nextByte;\n'
                  '              const int result = recv(fd, &nextByte, 1, MSG_PEEK | MSG_DONTWAIT);\n'
                  '              if (result >= 0) return result == 0 ? 0 : 1;\n'
                  '              return (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) ? 1 : '
                  '-1;\n'
                  '            });\n'
                  '        const size_t received = chunk.size;\n'
                  '        const char* failed = chunk.error;\n'
                  '        _currentRaw->currentSize = received;\n'
                  '        _currentRaw->totalSize += received;\n'
                  '        if (received != 0) {\n'
                  '          _currentRaw->status = RAW_WRITE;\n'
                  '          _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '        }\n'
                  '        // An adapter can reject/close during START or WRITE. Do not call\n'
                  '        // END or run response middleware after that explicit client stop.\n'
                  '        if (client.fd() < 0 && !failed) failed = "handler-closed";\n'
                  '        if (failed) {\n'
                  '          log_w("Raw body aborted: %s, received=%u", failed, '
                  'static_cast<unsigned>(_currentRaw->totalSize));\n'
                  '          _currentRaw->status = RAW_ABORTED;\n'
                  '          _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '          return false;\n'
                  '        }\n'
                  '      }\n'
                  '#else\n'
                  '      while (_currentRaw->totalSize < (size_t)_clientContentLength) {\n'
                  '        size_t read_len = std::min((size_t)_clientContentLength - _currentRaw->totalSize, '
                  '(size_t)HTTP_RAW_BUFLEN);\n'
                  '        _currentRaw->currentSize = client.readBytes(_currentRaw->buf, read_len);\n'),
                 ('        }\n'
                  '        _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '      }\n'
                  '      _currentRaw->status = RAW_END;\n'
                  '      _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '      log_v("Finish Raw");\n',
                  '        }\n'
                  '        _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '      }\n'
                  '#endif\n'
                  '      _currentRaw->status = RAW_END;\n'
                  '      _currentHandler->raw(*this, _currentUri, *_currentRaw);\n'
                  '      log_v("Finish Raw");\n'),
                 ('  _currentArgs = 0;\n'
                  '  if (data.length() == 0) {\n'
                  '    _currentArgCount = 0;\n'
                  '    _currentArgs = new RequestArgument[1];\n'
                  '    return;\n'
                  '  }\n',
                  '  _currentArgs = 0;\n'
                  '  if (data.length() == 0) {\n'
                  '    _currentArgCount = 0;\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '    if (_currentUri == "/upload/raw") {\n'
                  '      _currentArgs = new (std::nothrow) RequestArgument[1];\n'
                  '      if (!_currentArgs) { _currentArgCount = 0; return; }\n'
                  '    } else\n'
                  '#endif\n'
                  '    _currentArgs = new RequestArgument[1];\n'
                  '    return;\n'
                  '  }\n'),
                 ('  }\n'
                  '  log_v("args count: %d", _currentArgCount);\n'
                  '\n'
                  '  _currentArgs = new RequestArgument[_currentArgCount + 1];\n'
                  '  int pos = 0;\n'
                  '  int iarg;\n',
                  '  }\n'
                  '  log_v("args count: %d", _currentArgCount);\n'
                  '\n'
                  '#if defined(FLOWE_RAW_UPLOAD)\n'
                  '  if (_currentUri == "/upload/raw") {\n'
                  '    _currentArgs = new (std::nothrow) RequestArgument[_currentArgCount + 1];\n'
                  '    if (!_currentArgs) { _currentArgCount = 0; return; }\n'
                  '  } else\n'
                  '#endif\n'
                  '  _currentArgs = new RequestArgument[_currentArgCount + 1];\n'
                  '  int pos = 0;\n'
                  '  int iarg;\n')],
 'WebServer.cpp': [('#else\n'
                    '    _currentUpload.reset();\n'
                    '#endif\n'
                    '    _currentRaw.reset();\n'
                    '  }\n'
                    '\n'
                    '  if (callYield) {\n',
                    '#else\n'
                    '    _currentUpload.reset();\n'
                    '#endif\n'
                    '#if defined(FLOWE_RAW_UPLOAD)\n'
                    '    // Reserved at startup. Do not allocate again on a fragmented Wi-Fi heap.\n'
                    '    if (_currentRaw) {\n'
                    '      _currentRaw->status = RAW_START;\n'
                    '      _currentRaw->totalSize = 0;\n'
                    '      _currentRaw->currentSize = 0;\n'
                    '      _currentRaw->data = nullptr;\n'
                    '    }\n'
                    '#else\n'
                    '    _currentRaw.reset();\n'
                    '#endif\n'
                    '  }\n'
                    '\n'
                    '  if (callYield) {\n')]}

def patched_text(name, original):
    result = original
    for old, new in REPLACEMENTS[name]:
        if new in result:
            if result.count(new) != 1:
                raise RuntimeError(f"Duplicate raw patch: {name}")
            continue
        if result.count(old) != 1:
            raise RuntimeError(f"Changed Arduino 3.3.7 raw anchor: {name}")
        result = result.replace(old, new, 1)
    return result

def apply(framework, helper_path=None):
    import json
    framework = Path(framework)
    if json.loads((framework / "package.json").read_text()).get("version") != "3.3.7":
        raise RuntimeError("Raw upload requires Arduino 3.3.7")
    source = framework / "libraries/WebServer/src"
    # Validate every file before changing any file.
    updated = {name: patched_text(name, (source / name).read_text()) for name in REPLACEMENTS}
    helper = (Path(helper_path) if helper_path else Path(__file__).with_name("FloweRawReceive.h")).read_bytes()
    for name, text in updated.items():
        if (source / name).read_text() != text:
            (source / name).write_text(text)
    (source / "FloweRawReceive.h").write_bytes(helper)

if "Import" in globals():
    Import("env")
    apply(env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
          Path(env["PROJECT_DIR"]) / "tools/FloweRawReceive.h")
