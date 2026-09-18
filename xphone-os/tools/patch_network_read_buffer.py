"""Pinned Arduino 3.3.7: optional raw-upload receive-buffer growth.

Normal client reads and allocation size are unchanged. The caller opts in only
after accepting a large X4 raw upload. Growth preserves prefetched bytes; OOM
keeps the original buffer. Validate all anchors before changing any file.
"""
from pathlib import Path
import json

REPLACEMENTS = {'NetworkClient.cpp': [('  int peek() {\n',
                        '  // Main-task/raw-upload use only. Preserve prefetched bytes and '
                        'positions.\n'
                        '  // Failure leaves the original allocation and capacity usable '
                        'unchanged.\n'
                        '  bool grow(size_t size) {\n'
                        '    constexpr size_t kMaxRawReadBuffer = 5744;\n'
                        '    if (_failed || _fd < 0 || size == 0 || size > kMaxRawReadBuffer) '
                        'return false;\n'
                        '    if (size <= _size) return true;  // never shrink or discard buffered '
                        'data\n'
                        '    uint8_t* larger = static_cast<uint8_t*>(realloc(_buffer, size));\n'
                        '    if (!larger) return false;\n'
                        '    _buffer = larger;\n'
                        '    _size = size;\n'
                        '    return true;\n'
                        '  }\n'
                        '\n'
                        '  int peek() {\n'),
                       ('int NetworkClient::read(uint8_t *buf, size_t size) {',
                        'bool NetworkClient::floweGrowReadBuffer(size_t size) {\n'
                        '  return fd() >= 0 && _rxBuffer && _rxBuffer->grow(size);\n'
                        '}\n'
                        '\n'
                        'int NetworkClient::read(uint8_t *buf, size_t size) {')],
 'NetworkClient.h': [('  int read(uint8_t *buf, size_t size);',
                      '  int read(uint8_t *buf, size_t size);\n'
                      '  // Optional bounded growth for an accepted raw upload; normal reads '
                      'unchanged.\n'
                      '  bool floweGrowReadBuffer(size_t size);')]}

def patched_text(name, original):
    result = original
    present = [new in original for _, new in REPLACEMENTS[name]]
    if any(present) and not all(present):
        raise RuntimeError(f"Partial receive-buffer patch: {name}")
    if not any(present) and ("floweGrowReadBuffer" in original or
                             "bool grow(size_t size)" in original):
        raise RuntimeError(f"Changed receive-buffer patch: {name}")
    for old, new in REPLACEMENTS[name]:
        if new in result:
            if result.count(new) != 1:
                raise RuntimeError(f"Duplicate receive-buffer patch: {name}")
            continue
        if result.count(old) != 1:
            raise RuntimeError(f"Changed Arduino 3.3.7 receive-buffer anchor: {name}")
        result = result.replace(old, new, 1)
    return result

def apply(framework):
    framework = Path(framework)
    if json.loads((framework / "package.json").read_text()).get("version") != "3.3.7":
        raise RuntimeError("Receive-buffer growth requires Arduino 3.3.7")
    source = framework / "libraries/Network/src"
    updated = {name: patched_text(name, (source / name).read_text()) for name in REPLACEMENTS}
    for name, text in updated.items():
        if (source / name).read_text() != text:
            (source / name).write_text(text)

if "Import" in globals():
    Import("env")
    apply(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
