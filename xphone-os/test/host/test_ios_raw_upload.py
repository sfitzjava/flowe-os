"""Compile the exact Swift upload-selection and URL helpers with small host stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'ios/X4Companion/ReaderLibraryManager.swift').read_text()
helpers = source[source.index('    private struct UploadTarget {'):source.index('    /// Filenames the journey layer', source.index('    private struct UploadTarget {'))]
code = '''import Foundation
final class Harness {
    static var hostKey = "bond-A"
    var deviceHost = "reader"
    var sessionToken: String? = "owner"
    static let uploadPath = "/books"
    func fileURL(path: String, endpoint: String) -> URL? {
        URL(string: "http://reader/" + endpoint + "?path=/books")
    }
''' + helpers + '''
    func run() {
        assert(!usesRawUpload(4099))
        uploadTarget = UploadTarget(host: "reader", readerId: "AABBCCDDEEFF", raw: true, compact: true, bond: Self.hostKey)
        assert(usesRawUpload(4099))
        assert(currentUploadTarget?.compact == true)
        assert(!usesRawUpload(0))
        assert(!usesRawUpload(67108865))
        deviceHost = "other"
        assert(!usesRawUpload(4099))
        assert(currentUploadTarget == nil)
        deviceHost = "reader"
        Self.hostKey = "bond-B"
        assert(!usesRawUpload(4099))
        Self.hostKey = "bond-A"
        let url = uploadURL(name: "Café + 中文.fbp", raw: true)!
        assert(url.path == "/upload/raw")
        assert(url.absoluteString.contains("%2B"))
        let items = URLComponents(url: url, resolvingAgainstBaseURL: false)!.queryItems!
        assert(items.first(where: { $0.name == "name" })!.value == "Café + 中文.fbp")
        var request = URLRequest(url: url)
        identifyUpload(&request)
        assert(request.value(forHTTPHeaderField: "X-Flowe-Reader-Id") == "AABBCCDDEEFF")
        assert(request.value(forHTTPHeaderField: "X-Flowe-Token") == "owner")
        assert(uploadURL(name: "legacy", raw: false)!.path == "/upload")
        uploadTarget = nil
        assert(!usesRawUpload(4099))
    }
}
Harness().run()
print("Swift raw selection, identity, Unicode URL and fallback tests passed")
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path / 'main.swift').write_text(code)
    subprocess.run(['swiftc', '-swift-version', '5', str(path / 'main.swift'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
