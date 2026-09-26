"""Verify cache migration, disk recovery boundaries, and fixed administrator provisioning."""

import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]


def load_script(filename):
    """Import a helper without invoking its CLI or privileged operations."""
    spec = importlib.util.spec_from_file_location(filename, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cache = load_script("steamshine-steam-cache.py")
storage = load_script("steamshine-storage-helper.py")
provision = load_script("steamshine-provision-management.py")
UUID = "19d3483c-a24c-4c26-a7fc-e5d622399d1d"


class CacheTests(unittest.TestCase):
    """Use isolated directories to verify migration without touching actual Steam data."""

    def setUp(self):
        """Emulate separate home/library devices and a stopped Steam client."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.home = self.base / "home"
        self.steam = self.home / ".local/share/Steam"
        self.library = self.base / "volume/SteamLibrary"
        (self.steam / "steamapps").mkdir(parents=True)
        (self.library / "steamapps").mkdir(parents=True)
        (self.steam / "steamapps/libraryfolders.vdf").write_text(f'"libraryfolders" {{ "0" {{ "path" "{self.steam}" }} "1" {{ "path" "{self.library}" }} }}')
        self.source = self.library / "steamapps/shadercache"
        self.identifier = cache.library_id(self.library)
        active = mock.patch.object(cache, "steam_active", return_value=False)
        self.active = active.start()
        self.addCleanup(active.stop)
        device = mock.patch.object(cache, "on_home_storage", side_effect=lambda path, home: path.resolve().is_relative_to(home))
        device.start()
        self.addCleanup(device.stop)

    def assert_no_partial_copy(self):
        """Only the lock file may remain after a migration is abandoned."""
        storage_dir = self.steam / "InternalCache/SteamShine"
        self.assertEqual(sorted(entry.name for entry in storage_dir.iterdir()), [".configure.lock"])

    def test_existing_internal_links_are_healthy(self):
        """Existing manually configured links remain recognized and byte-for-byte unchanged."""
        destination = self.steam / "InternalCache/NVME2TB/shadercache"
        destination.mkdir(parents=True)
        self.source.symlink_to(destination)
        before = self.source.readlink()
        self.active.return_value = True
        result = cache.configure(self.home, self.identifier)
        self.assertFalse(result["changed"])
        self.assertEqual(before, self.source.readlink())

    def test_copies_cache_and_retains_original_and_proton(self):
        """A successful migration preserves the original contents and leaves saves untouched."""
        (self.source / "42").mkdir(parents=True)
        (self.source / "42/cache.bin").write_bytes(b"shader\0data")
        saves = self.library / "steamapps/compatdata/42"
        saves.mkdir(parents=True)
        (saves / "save.dat").write_text("saved game")
        result = cache.configure(self.home, self.identifier)
        self.assertTrue(self.source.is_symlink())
        self.assertEqual((self.source / "42/cache.bin").read_bytes(), b"shader\0data")
        self.assertEqual((Path(result["backup_path"]) / "42/cache.bin").read_bytes(), b"shader\0data")
        self.assertEqual((saves / "save.dat").read_text(), "saved game")
        self.assertFalse(cache.configure(self.home, self.identifier)["changed"])

    def test_missing_cache_can_be_created(self):
        """A registered mounted library can configure a cache before its first game launch."""
        result = cache.configure(self.home, self.identifier)
        self.assertTrue(result["changed"])
        self.assertEqual(result["backup_path"], "")
        self.assertTrue(self.source.is_dir())

    def test_active_steam_and_unknown_ids_are_rejected(self):
        """No files are created for active Steam, unregistered IDs, or command-like input."""
        self.active.return_value = True
        for identifier in (self.identifier, "f" * 24, "../../home", "abcd; id", ""):
            with self.subTest(identifier=identifier), self.assertRaises(ValueError):
                cache.configure(self.home, identifier)
        self.assertFalse(self.source.exists())

    def test_disconnected_library_is_never_recreated(self):
        """A stale library entry cannot create a substitute directory on the wrong disk."""
        (self.library / "steamapps").rmdir()
        self.library.rmdir()
        with self.assertRaisesRegex(ValueError, "mount"):
            cache.configure(self.home, self.identifier)
        self.assertFalse(self.library.exists())

    def test_broken_and_custom_links_are_preserved(self):
        """Existing external or broken links require manual review rather than replacement."""
        self.source.symlink_to(self.base / "absent")
        with self.assertRaisesRegex(ValueError, "link"):
            cache.configure(self.home, self.identifier)
        self.assertEqual(self.source.readlink(), self.base / "absent")
        (self.base / "absent").mkdir()
        with self.assertRaisesRegex(ValueError, "custom link"):
            cache.configure(self.home, self.identifier)

    def test_insufficient_space_leaves_original(self):
        """The migration reserves free space and does not rename an uncopied cache."""
        self.source.mkdir()
        (self.source / "data").write_text("original")
        with mock.patch.object(cache.shutil, "disk_usage", return_value=mock.Mock(free=1)), self.assertRaisesRegex(ValueError, "1 GiB"):
            cache.configure(self.home, self.identifier)
        self.assertFalse(self.source.is_symlink())
        self.assertEqual((self.source / "data").read_text(), "original")

    def test_copy_failure_preserves_original(self):
        """A partial copy cannot replace the active cache path."""
        self.source.mkdir()
        (self.source / "data").write_text("original")
        with mock.patch.object(cache.shutil, "copytree", side_effect=OSError("copy failed")), self.assertRaises(OSError):
            cache.configure(self.home, self.identifier)
        self.assertFalse(self.source.is_symlink())
        self.assertEqual((self.source / "data").read_text(), "original")
        self.assert_no_partial_copy()

    def test_symlinks_inside_cache_are_not_followed(self):
        """A nested link cannot make migration read or change an unrelated directory."""
        self.source.mkdir()
        (self.source / "outside").symlink_to(self.home)
        with self.assertRaisesRegex(ValueError, "links"):
            cache.configure(self.home, self.identifier)
        self.assertFalse(self.source.is_symlink())

    def test_steam_start_during_copy_stops_switch(self):
        """New Steam activity aborts before the original directory is renamed."""
        self.source.mkdir()
        self.active.side_effect = [False, False, True]
        with self.assertRaisesRegex(ValueError, "Steam started"):
            cache.configure(self.home, self.identifier)
        self.assertFalse(self.source.is_symlink())
        self.assert_no_partial_copy()

    def test_link_failure_rolls_back_source(self):
        """A link-creation failure restores the original directory and all of its data."""
        self.source.mkdir()
        (self.source / "data").write_text("original")
        with mock.patch.object(Path, "symlink_to", side_effect=OSError("link failed")), self.assertRaises(OSError):
            cache.configure(self.home, self.identifier)
        self.assertEqual((self.source / "data").read_text(), "original")
        self.assertFalse(self.source.is_symlink())

    def test_keyvalues_parser_and_alias_deduplication(self):
        """Quoted braces and escaped text are data; malformed documents fail closed."""
        self.assertEqual(cache.parse_vdf('"key" "a { b }" // comment\n'), {"key": "a { b }"})
        for text in ('"key" {', 'garbage', '"key" }', '}'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                cache.parse_vdf(text)
        self.assertEqual(len(cache.libraries(self.steam)), 2)


class StorageTests(unittest.TestCase):
    """Verify UUID, mount-path, and filesystem boundaries with all system actions mocked."""

    def setUp(self):
        """Create an isolated recovery environment and replace only external system commands."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.record = {"uuid": UUID, "target": "/run/media/deck/Samsung2TB", "fstype": "ext4", "read_only": False, "origin": "fixture"}
        for name, path in (("STATE", self.base / "state/mounts.json"), ("BACKUPS", self.base / "backups"), ("FSTAB", self.base / "fstab"), ("UNITS", self.base / "units")):
            patch = mock.patch.object(storage, name, path)
            patch.start()
            self.addCleanup(patch.stop)
        storage.BACKUPS.mkdir()
        storage.UNITS.mkdir()
        storage.FSTAB.write_text("")
        trust = mock.patch.object(storage, "trusted", side_effect=lambda path: path.is_file() and not path.is_symlink())
        trust.start()
        self.addCleanup(trust.stop)

    def test_only_uuid_ext4_data_volumes_are_accepted(self):
        """System mountpoints, option injection, and unsupported filesystem layouts are excluded."""
        good = f"UUID={UUID} /run/media/deck/Samsung2TB ext4 defaults,nofail 0 2"
        self.assertEqual(storage.fstab_records(good, "fixture"), [self.record])
        for line in (good.replace("/run/media/deck/Samsung2TB", "/home"), good.replace("UUID=" + UUID, "/dev/sda1"), good.replace("ext4", "btrfs"), good.replace("defaults,nofail", "bind")):
            self.assertEqual(storage.fstab_records(line, "fixture"), [])
        for path in ("/", "/run/media", "/run/media/../etc", "/run/media/disk\n[Service]", "/run/media/%n", "/home/deck"):
            self.assertFalse(storage.safe_target(path))

    def test_reads_os_backup_without_extracting_files(self):
        """Only etc/fstab is read from an archive; arbitrary archive paths are not extracted."""
        archive = storage.BACKUPS / "2026-09-24.tar.xz"
        payload = f"UUID={UUID} /run/media/deck/Samsung2TB ext4 defaults 0 2".encode()
        with tarfile.open(archive, "w:xz") as output:
            member = tarfile.TarInfo("etc/fstab")
            member.size = len(payload)
            output.addfile(member, io.BytesIO(payload))
        records = storage.known_records([])
        self.assertEqual(records[UUID]["target"], self.record["target"])
        self.assertFalse((self.base / "etc").exists())

    def test_system_device_uuid_is_excluded(self):
        """An archived data path cannot authorize mounting the current OS filesystem."""
        storage.FSTAB.write_text(f"UUID={UUID} /run/media/deck/old ext4 defaults 0 2")
        self.assertEqual(storage.known_records([{"uuid": UUID, "target": "/home", "fstype": "ext4"}]), {})

    def test_temporary_mount_does_not_replace_saved_path(self):
        """A manual file-manager mount must not overwrite the path games expect after recovery."""
        storage.FSTAB.write_text(f"UUID={UUID} /run/media/deck/Samsung2TB ext4 defaults 0 2")
        records = storage.known_records([{"uuid": UUID, "target": "/run/media/deck/NVME2TB", "fstype": "ext4"}])
        self.assertEqual(records[UUID]["target"], "/run/media/deck/Samsung2TB")

    def test_enabled_unit_for_other_uuid_is_not_healthy(self):
        """Unit enablement alone cannot establish that a saved volume will mount correctly."""
        (storage.UNITS / "fixture.mount").write_text("[Mount]\nWhat=/dev/disk/by-uuid/00000000-0000-0000-0000-000000000000\nWhere=/run/media/deck/Samsung2TB\nType=ext4\n")
        with mock.patch.object(storage, "unit_name", return_value="fixture.mount"), mock.patch.object(storage, "command") as run:
            self.assertFalse(storage.persistent(self.record))
            run.assert_not_called()

    def test_duplicate_uuid_or_disconnected_device_cannot_restore(self):
        """The helper refuses to guess which device a duplicated UUID identifies."""
        device = {"uuid": UUID, "path": "/dev/example", "fstype": "ext4", "ro": False}
        with mock.patch.object(storage, "known_records", return_value={UUID: self.record}), mock.patch.object(storage, "mounted_filesystems", return_value=[]), mock.patch.object(storage, "persistent", return_value=False):
            for devices in ([], [device, device], [device | {"fstype": "xfs"}], [device | {"ro": True}]):
                with mock.patch.object(storage, "discovered_devices", return_value=devices):
                    self.assertFalse(storage.snapshot()["volumes"][0]["can_restore"])

    def test_nonempty_and_symlinked_mount_targets_are_rejected(self):
        """Recovery cannot hide existing user files or redirect a mount through a symlink."""
        target = self.base / "mount"
        target.mkdir()
        (target / "important").write_text("keep")
        with self.assertRaisesRegex(ValueError, "contains files"):
            storage.validate_target(target, False)
        alias = self.base / "alias"
        alias.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "symbolic"):
            storage.validate_target(alias, False)

    def test_failed_readonly_check_never_mounts_or_writes_unit(self):
        """Filesystem errors stop recovery before any system configuration is changed."""
        record = self.record | {"target": str(self.base / "mount"), "mounted": False, "persistent": False, "can_restore": True}
        def run(arguments, **kwargs):
            """Return a matching UUID and a failed check without invoking real disk utilities."""
            if arguments[0].endswith("lsblk"):
                return subprocess.CompletedProcess(arguments, 0, json.dumps({"blockdevices": [{"uuid": UUID, "fstype": "ext4"}]}), "")
            self.assertEqual(arguments[:2], ["/usr/bin/e2fsck", "-fn"])
            return subprocess.CompletedProcess(arguments, 4, "filesystem errors", "")
        with mock.patch.object(storage, "snapshot", return_value={"volumes": [record]}), mock.patch.object(storage, "command", side_effect=run), self.assertRaisesRegex(ValueError, "check did not pass"):
            storage.restore(UUID)
        self.assertEqual(list(storage.UNITS.iterdir()), [])

    def test_remember_saves_uuid_mapping_outside_etc(self):
        """A persistent recovery copy retains exact paths and only validated records."""
        with mock.patch.object(storage, "known_records", return_value={UUID: self.record}), mock.patch.object(storage, "mounted_filesystems", return_value=[]):
            self.assertTrue(storage.remember()["success"])
        self.assertEqual(json.loads(storage.STATE.read_text())["mounts"], [self.record])

    def test_successful_restore_creates_only_expected_mount_unit(self):
        """A checked UUID is restored at its saved path with safe persistent mount options."""
        record = self.record | {"target": str(self.base / "mount"), "mounted": False, "persistent": False, "can_restore": True}
        calls = []
        def run(arguments, **kwargs):
            """Simulate a healthy check and mount while recording the exact operation boundary."""
            calls.append(arguments)
            output = ""
            if arguments[0].endswith("lsblk"):
                output = json.dumps({"blockdevices": [{"uuid": UUID, "fstype": "ext4"}]})
            elif arguments[0].endswith("systemd-escape"):
                output = "fixture.mount\n"
            return subprocess.CompletedProcess(arguments, 0, output, "")
        snapshots = [{"volumes": [record]}, {"volumes": [record | {"mounted": True, "persistent": True}]}]
        with mock.patch.object(storage, "snapshot", side_effect=snapshots), mock.patch.object(storage, "command", side_effect=run), mock.patch.object(storage, "mounted_filesystems", return_value=[]), mock.patch.object(storage, "remember"):
            self.assertTrue(storage.restore(UUID)["success"])
        unit = (storage.UNITS / "fixture.mount").read_text()
        self.assertIn(f"What=/dev/disk/by-uuid/{UUID}", unit)
        self.assertIn("nosuid,nodev,errors=remount-ro", unit)
        self.assertIn(["/usr/bin/e2fsck", "-fn", f"/dev/disk/by-uuid/{UUID}"], calls)
        self.assertIn(["/usr/bin/systemctl", "enable", "--now", "fixture.mount"], calls)

    def test_already_restored_is_noop(self):
        """Repeated requests do not recheck, remount, or alter a healthy registered mount."""
        with mock.patch.object(storage, "snapshot", return_value={"volumes": [self.record | {"mounted": True, "persistent": True}]}), mock.patch.object(storage, "command") as run:
            self.assertTrue(storage.restore(UUID)["success"])
            run.assert_not_called()


class ProvisioningTests(unittest.TestCase):
    """Keep the authorization boundary limited to fixed helpers, never a root shell."""

    def test_policy_rejects_username_injection(self):
        """Sudoers user syntax cannot be supplied as an account name."""
        for name in ("root", "", "deck ALL=(ALL) ALL", "deck\nroot", "deck,root"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                provision.policy_for(name)

    def test_policy_grants_only_fixed_helper_commands(self):
        """The policy enables bounded storage, Decky and GPU operations, not generic execution."""
        policy = provision.policy_for("deck")
        self.assertIn("Defaults!STEAMSHINE_WEB_MANAGEMENT !authenticate", policy)
        self.assertIn("steamshine-storage-helper restore *", policy)
        self.assertNotIn("/usr/bin/bash", policy)
        self.assertNotIn("/usr/bin/python", policy)
        self.assertNotIn("NOPASSWD: ALL", policy)


if __name__ == "__main__":
    unittest.main()
