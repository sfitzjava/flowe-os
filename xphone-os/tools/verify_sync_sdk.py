"""Fail closed if the explicit fast build selects an unmeasured SDK artifact."""
from pathlib import Path
import hashlib
import json

def verify(package, manifest=None):
    package = Path(package)
    manifest = manifest or Path(__file__).with_name("sync-fast-sdk.json")
    expected = json.loads(Path(manifest).read_text())
    for name, digest in expected["sha256"].items():
        path = package / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Fast sync SDK provenance mismatch: {name}; use the standard build")
    print(f"[sync-sdk] verified {len(expected['sha256'])} artifacts; TCP_WND=17232 TCP_SND_BUF=17232")

if "Import" in globals():
    Import("env")
    verify(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"),
           Path(env["PROJECT_DIR"]) / "tools/sync-fast-sdk.json")
