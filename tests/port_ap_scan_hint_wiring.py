"""Production wiring guard for the artifact scan hint (issue #170).

Scanning an Artifact Temple totem asks the Archipelago server to hint the
artifact. The totem's text is built when its STRG loads (room load) and again
whenever the table refreshes, so the request must hang off the scan finishing
in CPlayer, never off the text path.

It is a source check: no engine code runs here.
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, braced, root_argument  # noqa: E402

root = root_argument()
PLAYER = root / "src/MetroidPrime/Player/CPlayer.cpp"
HINTS = root / "platform/port_hints.cpp"
CLIENT = root / "platform/port_apclient.cpp"


class Wiring(WiringCase):
    def test_scan_completion_fires_hint(self):
        body = braced(PLAYER.read_text(), "void CPlayer::UpdateScanningState(", str(PLAYER))
        hook = body.index("PortHints::ScanCompleted(")
        self.assertLess(hook, body.index("SetScanningState(kSS_ScanComplete, mgr);"))

    def test_text_path_queues_nothing(self):
        text = braced(HINTS.read_text(), "bool TotemText(", str(HINTS))
        self.assertNotIn("ArtifactScanned", text)
        client = braced(CLIENT.read_text(), "bool ArtifactHint(", str(CLIENT))
        self.assertNotIn("pendingHintScouts", client)
        self.assertNotIn("TakeArtifactScanHint", client)

    def test_scan_path_queues(self):
        scanned = braced(CLIENT.read_text(), "void ArtifactScanned(", str(CLIENT))
        self.assertIn("pendingHintScouts.push_back", scanned)


if __name__ == "__main__":
    unittest.main(verbosity=2)
