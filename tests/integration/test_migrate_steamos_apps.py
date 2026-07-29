#!/usr/bin/env python3
"""Tests for safe migration of existing Sunshine applications."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).parents[2] / "scripts" / "migrate-steamos-apps.py"
SPEC = importlib.util.spec_from_file_location("migrate_steamos_apps", SCRIPT)
assert SPEC and SPEC.loader
MIGRATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MIGRATION)


class MigrateSteamOSAppsTests(unittest.TestCase):
    """Verify targeted, atomic, and idempotent application migration."""

    def setUp(self) -> None:
        """Create an existing user applications file with custom fields."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.apps_path = Path(self.temporary.name) / "apps.json"
        self.original = {
            "env": {"CUSTOM": "preserved"},
            "apps": [
                {"name": "Desktop", "image-path": "custom-desktop.png"},
                {
                    "name": "Steam Big Picture",
                    "detached": ["custom-steam-command"],
                    "prep-cmd": [{"do": "custom-do", "undo": "custom-undo"}],
                },
                {"name": "Custom Game", "cmd": "custom-game"},
            ],
        }
        self.apps_path.write_text(json.dumps(self.original), encoding="utf-8")
        self.apps_path.chmod(0o600)

    def test_migrates_targets_and_preserves_custom_configuration(self) -> None:
        """Add hooks only to target applications and retain an exact backup."""
        self.assertTrue(MIGRATION.migrate(self.apps_path))

        migrated = json.loads(self.apps_path.read_text(encoding="utf-8"))
        backup = json.loads((Path(str(self.apps_path) + ".steamshine-backup")).read_text(encoding="utf-8"))
        self.assertEqual(backup, self.original)
        self.assertEqual(migrated["env"], self.original["env"])
        self.assertEqual(migrated["apps"][0]["image-path"], "custom-desktop.png")
        self.assertEqual(migrated["apps"][0]["prep-cmd"][0], MIGRATION.DISPLAY_HOOK)
        self.assertEqual(migrated["apps"][1]["detached"], ["custom-steam-command"])
        self.assertEqual(migrated["apps"][1]["prep-cmd"][1], {"do": "custom-do", "undo": "custom-undo"})
        self.assertEqual(migrated["apps"][2], self.original["apps"][2])
        self.assertEqual(self.apps_path.stat().st_mode & 0o777, 0o600)

    def test_is_idempotent(self) -> None:
        """Leave an already migrated applications file unchanged."""
        self.assertTrue(MIGRATION.migrate(self.apps_path))
        first = self.apps_path.read_bytes()
        self.assertFalse(MIGRATION.migrate(self.apps_path))
        self.assertEqual(self.apps_path.read_bytes(), first)

    def test_removes_fixed_display_only_from_legacy_steam_command(self) -> None:
        """Replace known legacy defaults without changing custom commands."""
        payload = {
            "apps": [
                {
                    "name": "Steam Big Picture",
                    "detached": [
                        "setsid env DISPLAY=:1 steam steam://open/bigpicture",
                        "setsid env DISPLAY=:0 steam steam://open/bigpicture",
                        "custom-steam-command",
                    ],
                },
                {
                    "name": "Custom Game",
                    "detached": ["setsid env DISPLAY=:1 steam steam://open/bigpicture"],
                },
            ]
        }
        self.apps_path.write_text(json.dumps(payload), encoding="utf-8")

        self.assertTrue(MIGRATION.migrate(self.apps_path))

        migrated = json.loads(self.apps_path.read_text(encoding="utf-8"))
        self.assertEqual(
            migrated["apps"][0]["detached"],
            [
                MIGRATION.STEAM_BIG_PICTURE_COMMAND,
                MIGRATION.STEAM_BIG_PICTURE_COMMAND,
                "custom-steam-command",
            ],
        )
        self.assertEqual(migrated["apps"][1]["detached"], payload["apps"][1]["detached"])

    def test_missing_file_is_ignored(self) -> None:
        """Allow Sunshine to create its packaged defaults when no file exists."""
        self.apps_path.unlink()
        self.assertFalse(MIGRATION.migrate(self.apps_path))

    def test_rejects_invalid_structure_without_rewriting(self) -> None:
        """Reject malformed application structure and preserve its bytes."""
        invalid = b'{"apps":"invalid"}\n'
        self.apps_path.write_bytes(invalid)
        with self.assertRaises(ValueError):
            MIGRATION.migrate(self.apps_path)
        self.assertEqual(self.apps_path.read_bytes(), invalid)

    def test_rejects_invalid_target_commands_without_rewriting(self) -> None:
        """Reject a non-list target preparation value before creating a backup."""
        invalid = b'{"apps":[{"name":"Desktop","prep-cmd":"invalid"}]}\n'
        self.apps_path.write_bytes(invalid)

        with self.assertRaises(ValueError):
            MIGRATION.migrate(self.apps_path)

        self.assertEqual(self.apps_path.read_bytes(), invalid)
        self.assertFalse(Path(str(self.apps_path) + ".steamshine-backup").exists())

    def test_rejects_invalid_detached_commands_without_rewriting(self) -> None:
        """Reject malformed Steam commands before creating a backup."""
        invalid = b'{"apps":[{"name":"Steam Big Picture","detached":"invalid"}]}\n'
        self.apps_path.write_bytes(invalid)

        with self.assertRaises(ValueError):
            MIGRATION.migrate(self.apps_path)

        self.assertEqual(self.apps_path.read_bytes(), invalid)
        self.assertFalse(Path(str(self.apps_path) + ".steamshine-backup").exists())


if __name__ == "__main__":
    unittest.main()
