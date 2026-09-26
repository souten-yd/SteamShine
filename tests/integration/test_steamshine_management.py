#!/usr/bin/env python3
"""Verify fixed administrator provisioning without modifying host policy or helpers."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "management", Path(__file__).resolve().parents[2] / "scripts/steamshine-provision-management.py"
)
management = importlib.util.module_from_spec(spec)
spec.loader.exec_module(management)


class ManagementTests(unittest.TestCase):
    """Exercise policy boundaries and atomic helper replacement."""

    def test_policy_is_fixed_and_rejects_invalid_callers(self):
        """Never interpolate sudoers directives from an unchecked account name."""
        policy = management.policy_for("deck")
        self.assertIn("probe-gpu", policy)
        self.assertIn("apply-profile *", policy)
        self.assertNotIn("/usr/bin/python3", policy)
        self.assertNotIn("storage", policy)
        for caller in ("root", "", "deck ALL", "deck\nALL", "a/b"):
            with self.assertRaises(ValueError):
                management.policy_for(caller)

    def test_replacement_retains_previous_and_rejects_symlink(self):
        """A failed destination check must leave the original helper unchanged."""
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "helper"
            management.install_file(target, b"old", 0o755)
            management.install_file(target, b"new", 0o755)
            self.assertEqual(target.read_bytes(), b"new")
            self.assertEqual(target.with_name("helper.previous").read_bytes(), b"old")
            self.assertEqual(target.stat().st_mode & 0o777, 0o755)
            with patch.object(management.os, "chown"):
                management.install_file(target, b"new", 0o755)
            link = Path(directory) / "link"
            link.symlink_to(target)
            with self.assertRaises(ValueError):
                management.install_file(link, b"bad", 0o755)
            self.assertEqual(target.read_bytes(), b"new")

    def test_provision_requires_root(self):
        """No files may be touched before sudo authentication succeeds."""
        with patch.object(management.os, "geteuid", return_value=1000):
            with self.assertRaises(ValueError):
                management.provision()


if __name__ == "__main__":
    unittest.main()
