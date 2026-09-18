"""Execute the exact Swift status probe/decoder with a deterministic HTTP peer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]

class IosReadinessTest(unittest.TestCase):
    def test_owned_probe_flow(self):
        source = (ROOT / "ios/X4Companion/ReaderLibraryManager.swift").read_text()
        fields = source[source.index("    private struct UploadTarget {"):source.index("    private func usesRawUpload(")]
        start = source.index("    private func probeStatus(host:")
        end = source.index("    /// End the Wi-Fi session.", start)
        methods = source[start:end]
        code = r'''import Foundation
struct ReaderLog { static func write(_ value: String) {} }
@MainActor final class Peer {
    var replies: [(String, Int)] = []
    var requests: [URLRequest] = []
    var onRequest: ((URLRequest) -> Void)?
    func receive(_ request: URLRequest) -> (Data, URLResponse) {
        onRequest?(request)
        requests.append(request)
        let reply = replies.removeFirst()
        return (Data(reply.0.utf8), HTTPURLResponse(url: request.url!, statusCode: reply.1, httpVersion: nil, headerFields: nil)!)
    }
    func data(from url: URL) async throws -> (Data, URLResponse) { receive(URLRequest(url: url)) }
    func data(for request: URLRequest) async throws -> (Data, URLResponse) { receive(request) }
}
@MainActor final class Harness {
    static var hostKey = "bond-A"
    var sessionToken: String? = "owner"
    var deviceHost = "candidate.local"
    var connectedDeviceModel: String? = "X4"
    var deviceFirmware: String? = nil
    var rescueProbeActive = false
    let session = Peer()
''' + fields + methods + r'''
    let publicStatus = #"{"readerId":"AABBCCDDEEFF","device":"X4","ip":"192.0.2.88"}"#
    let readyStatus = #"{"readerId":"AABBCCDDEEFF","device":"X4","ip":"192.0.2.88","rawUploadVersion":1,"compactPageVersion":1}"#
    func reset() {
        Self.hostKey = "bond-A"; sessionToken = "owner"; deviceHost = "candidate.local"
        uploadTarget = nil; connectedDeviceModel = "X4"; rescueProbeActive = false
        session.requests = []; session.replies = []; session.onRequest = nil
    }
    func run() async {
        reset(); session.replies = [(publicStatus,200),(readyStatus,200)]
        session.onRequest = { [self] _ in assert(uploadTarget == nil) }
        let found = await probeStatus(host: "candidate.local")
        assert(found == "192.0.2.88" && session.requests.count == 2)
        assert(session.requests[0].value(forHTTPHeaderField: "X-Flowe-Token") == nil)
        assert(session.requests[1].url!.host == "candidate.local")
        assert(session.requests[1].value(forHTTPHeaderField: "X-Flowe-Token") == "owner")
        assert(session.requests[1].value(forHTTPHeaderField: "X-Flowe-Reader-Id") == "AABBCCDDEEFF")
        assert(uploadTarget?.raw == true && uploadTarget?.compact == true)
        deviceHost = found!; assert(currentUploadTarget != nil)

        reset(); uploadTarget = UploadTarget(host: deviceHost, readerId: "001122334455", raw: true, compact: true, bond: Self.hostKey)
        session.replies = [(publicStatus,200)]
        let wrongKnown = await probeStatus(host: "candidate.local")
        assert(wrongKnown == nil && session.requests.count == 1 && uploadTarget == nil)
        assert(session.requests.allSatisfy { $0.value(forHTTPHeaderField: "X-Flowe-Token") == nil })

        reset(); session.replies = [(publicStatus,200),(#"{"readerId":"001122334455","rawUploadVersion":1,"compactPageVersion":1}"#,200)]
        let changedReader = await probeStatus(host: "candidate.local")
        assert(changedReader == nil && uploadTarget == nil)

        reset(); rescueProbeActive = true
        session.replies = [(#"{"readerId":"AABBCCDDEEFF","device":"X3"}"#,200)]
        let wrongModel = await probeStatus(host: "candidate.local")
        assert(wrongModel == nil && session.requests.count == 1)

        reset(); session.replies = [(readyStatus,200),("not ready",503)]
        let failedReady = await probeStatus(host: "candidate.local")
        assert(failedReady == nil && uploadTarget == nil)

        reset(); session.replies = [(readyStatus,200),(publicStatus,200)]
        let noCapability = await probeStatus(host: "candidate.local")
        assert(noCapability != nil && uploadTarget?.raw == false && uploadTarget?.compact == false)

        reset(); session.replies = [(publicStatus,200)]
        session.onRequest = { _ in Self.hostKey = "bond-B" }
        let changedBond = await probeStatus(host: "candidate.local")
        assert(changedBond == nil && session.requests.count == 1 && uploadTarget == nil)

        reset(); session.replies = [(publicStatus,200)]
        session.onRequest = { [self] _ in sessionToken = "changed" }
        let changedToken = await probeStatus(host: "candidate.local")
        assert(changedToken == nil && session.requests.count == 1 && uploadTarget == nil)

        reset(); session.replies = [(#"{"device":"X4","ip":"192.0.2.88"}"#,200)]
        let legacy = await probeStatus(host: "candidate.local")
        assert(legacy == "192.0.2.88" && session.requests.count == 1 && uploadTarget == nil)

        reset(); sessionToken = nil; session.replies = [(publicStatus,200)]
        let guest = await probeStatus(host: "candidate.local")
        assert(guest != nil && session.requests.count == 1 && uploadTarget?.raw == false)
        print("PASS: exact Swift owned preflight ordering, authority, host, identity, capability and legacy cases")
    }
}
@main struct Main { @MainActor static func main() async { await Harness().run() } }
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / "probe.swift").write_text(code)
            subprocess.run(["swiftc", "-swift-version", "5", "-parse-as-library", str(path / "probe.swift"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

if __name__ == "__main__": unittest.main()
