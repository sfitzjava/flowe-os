"""Run the Swift active-session branches and check the matching Android boundary.

The extracted prefix is production code. New-session route work is outside
this test. It checks that enqueueing work preserves a negotiated upload target.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]

class SessionRetentionTests(unittest.TestCase):
    def test_swift_active_and_new_sessions(self):
        source = (ROOT / 'ios/X4Companion/ReaderLibraryManager.swift').read_text()
        start = source.index('    private func startSession(direct: Bool) {')
        end = source.index('            reconnectBudgetTask?.cancel()', start)
        method = source[start:end] + '            break\n        }\n    }\n'
        harness = '''enum Transfer { case running, starting, connecting, syncing, idle, failed, reconnecting }
final class Harness {
    var transfer: Transfer = .idle
    var uploadTarget: String? = "verified-reader"
    var drained = 0
    func drainPendingWork() { drained += 1 }
''' + method + '''
    func check() {
        for state in [Transfer.running, .starting, .connecting, .syncing] {
            transfer = state
            uploadTarget = "verified-reader"
            startSession(direct: false)
            assert(uploadTarget == "verified-reader")
        }
        assert(drained == 1)
        for state in [Transfer.idle, .failed, .reconnecting] {
            transfer = state
            uploadTarget = "old-reader"
            startSession(direct: false)
            assert(uploadTarget == nil)
        }
    }
}
Harness().check()
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'main.swift').write_text(harness)
            subprocess.run(['swiftc', str(path / 'main.swift'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_android_clear_belongs_only_to_new_session_branch(self):
        source = (ROOT / 'android/app/src/main/kotlin/ink/flowe/companion/reader/ReaderLibraryManager.kt').read_text()
        start = source.index('    private fun startSession(direct: Boolean) {')
        end = source.index('                val generation = ++sessionGeneration', start)
        prefix = source[start:end]
        branch = 'is DeviceTransfer.Idle, is DeviceTransfer.Failed, is DeviceTransfer.Reconnecting -> {'
        before, after = prefix.split(branch)
        self.assertNotIn('clearUploadCapability()', before)
        self.assertEqual(after.strip(), 'http.clearUploadCapability()')
        self.assertIn('is DeviceTransfer.Running -> drainPendingWork()', before)
        self.assertIn('is DeviceTransfer.Starting, is DeviceTransfer.Connecting, is DeviceTransfer.Syncing ->', before)

if __name__ == '__main__':
    unittest.main()
