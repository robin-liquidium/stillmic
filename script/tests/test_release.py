import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "script"))

import release


class ReleaseTests(unittest.TestCase):
    def test_changed_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(release, "OUT", Path(directory)), patch.object(
            release, "github"
        ):
            path = Path(directory) / "app.zip"
            path.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                release.fetch("v1.0.0", "app.zip", "0" * 64)

    def test_submission_intent_is_saved_before_apple_call(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.zip"
            path.write_bytes(b"exact bytes")
            state = {"phase": "build"}
            saved = []

            def save(_tag, value):
                saved.append(json.loads(json.dumps(value)))

            with patch.object(release, "save", side_effect=save), patch.object(
                release, "notary", side_effect=TimeoutError
            ):
                with self.assertRaises(TimeoutError):
                    release.submit("v1.0.0", state, "app", path)
            self.assertEqual(saved[0]["phase"], "app_submitting")
            self.assertEqual(saved[0]["app"]["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertNotIn("id", saved[0]["app"])

    def test_pending_apple_result_does_not_advance(self):
        state = {"phase": "app_pending", "app": {"id": "existing-id"}}
        with patch.object(release, "notary", return_value={"status": "In Progress"}) as notary, patch.object(
            release, "save"
        ) as save:
            self.assertFalse(release.accepted("v1.0.0", state, "app"))
            save.assert_not_called()
            notary.assert_called_once_with("info", "existing-id")

    def test_rejection_is_persisted(self):
        state = {"phase": "dmg_pending", "dmg": {"id": "existing-id"}}
        with patch.object(release, "notary", return_value={"status": "Invalid"}), patch.object(
            release, "save"
        ) as save:
            with self.assertRaises(RuntimeError):
                release.accepted("v1.0.0", state, "dmg")
            self.assertEqual(state["phase"], "dmg_rejected")
            save.assert_called_once()

    def test_appcast_requires_signing_key(self):
        with patch.dict(release.os.environ, {}, clear=True), tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "SPARKLE_PRIVATE_KEY"):
                release.generate_appcast("v1.0.0", Path(directory) / "Stillmic-1.0.0.dmg", {"changes": ["Test"]})


if __name__ == "__main__":
    unittest.main()
