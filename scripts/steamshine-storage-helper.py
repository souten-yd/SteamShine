#!/usr/bin/python3
"""Recover previously registered data-volume mounts without formatting or repairing disks."""

from __future__ import annotations

import argparse
import configparser
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tarfile
import tempfile

STATE = Path("/var/lib/steamshine/storage/mounts.json")
BACKUPS = Path("/var/lib/steamos-atomupd/etc_backup")
FSTAB = Path("/etc/fstab")
UNITS = Path("/etc/systemd/system")
UUID_PATTERN = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
DATA_ROOTS = (Path("/run/media"), Path("/media"), Path("/mnt"), Path("/var/mnt"))


def command(arguments: list[str], timeout: int = 15, check: bool = True) -> subprocess.CompletedProcess:
    """Run a fixed executable without a shell, bounding runtime and reporting failures."""
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout, env={"PATH": "/usr/bin:/usr/sbin", "LC_ALL": "C"})
    if check and result.returncode:
        raise ValueError((result.stderr or result.stdout or "Storage operation failed.")[-4096:].strip())
    return result


def trusted(path: Path) -> bool:
    """Only accept regular configuration files owned by root and not writable by other users."""
    try:
        info = path.lstat()
        return stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022
    except OSError:
        return False


def safe_target(value: str) -> bool:
    """Allow only ordinary data-volume paths, never system mounts or unit-file syntax."""
    if not isinstance(value, str) or not value.startswith("/") or value != value.strip():
        return False
    if any(character in value for character in ("\n", "\r", "\0", "\\", "%")):
        return False
    path = Path(value)
    if ".." in path.parts or str(path) != value:
        return False
    return any(path != base and path.is_relative_to(base) for base in DATA_ROOTS)


def valid_record(record: dict) -> bool:
    """Accept an explicit UUID/ext4/data-path mapping; unsupported filesystems remain untouched."""
    return isinstance(record, dict) and bool(UUID_PATTERN.fullmatch(str(record.get("uuid", "")))) and record.get("fstype") == "ext4" and safe_target(record.get("target", ""))


def fstab_records(text: str, origin: str) -> list[dict]:
    """Read only UUID-based ext4 data-volume entries from a trusted fstab snapshot."""
    result = []
    for line in text.splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#") or len(fields) < 4 or not fields[0].startswith("UUID="):
            continue
        target = fields[1].replace(r"\040", " ").replace(r"\011", "\t")
        options = fields[3].split(",")
        if any(option in options for option in ("bind", "rbind", "noauto")):
            continue
        record = {"uuid": fields[0][5:].lower(), "target": target, "fstype": fields[2], "read_only": "ro" in options, "origin": origin}
        if valid_record(record):
            result.append(record)
    return result


def flatten(entries: list[dict]) -> list[dict]:
    """Flatten lsblk or findmnt JSON trees while preserving each device/mount record."""
    result = []
    for entry in entries:
        result.append(entry)
        result.extend(flatten(entry.get("children", [])))
    return result


def mounted_filesystems() -> list[dict]:
    """Read active mounts and UUIDs without probing or modifying filesystem contents."""
    output = command(["/usr/bin/findmnt", "--json", "--real", "--output", "SOURCE,TARGET,FSTYPE,UUID,OPTIONS"]).stdout
    return flatten(json.loads(output).get("filesystems", []))


def discovered_devices() -> list[dict]:
    """Read the kernel/udev block-device inventory, including disconnected-volume absence."""
    output = command(["/usr/bin/lsblk", "--json", "--bytes", "--output", "PATH,TYPE,UUID,FSTYPE,LABEL,SIZE,RO"]).stdout
    return flatten(json.loads(output).get("blockdevices", []))


def known_records(mounts: list[dict]) -> dict[str, dict]:
    """Recover mount mappings from OS backups, the persistent ledger, and active data mounts."""
    records = {}
    for archive in sorted(BACKUPS.glob("*.tar.xz")):
        if not trusted(archive) or archive.stat().st_size > 64 * 1024 * 1024:
            continue
        try:
            with tarfile.open(archive, "r:xz") as backup:
                member = backup.getmember("etc/fstab")
                if not member.isfile() or member.size > 1024 * 1024:
                    continue
                contents = backup.extractfile(member).read().decode("utf-8")
                for record in fstab_records(contents, f"SteamOS backup {archive.name}"):
                    records[record["uuid"]] = record
        except (KeyError, OSError, tarfile.TarError, UnicodeError):
            continue
    if trusted(STATE):
        for record in json.loads(STATE.read_text()).get("mounts", []):
            if valid_record(record):
                records[record["uuid"]] = record
    if trusted(FSTAB):
        for record in fstab_records(FSTAB.read_text(), "Current fstab"):
            records[record["uuid"]] = record
    protected = {item.get("uuid") for item in mounts if item.get("uuid") and not safe_target(item.get("target", ""))}
    for item in mounts:
        record = {"uuid": item.get("uuid", ""), "target": item.get("target", ""), "fstype": item.get("fstype"), "read_only": "ro" in item.get("options", "").split(","), "origin": "Saved mount settings"}
        if valid_record(record) and record["uuid"] not in protected:
            records.setdefault(record["uuid"], record)
    return {key: record for key, record in records.items() if key not in protected}


def unit_name(target: str) -> str:
    """Ask systemd to escape the already validated mount path into its exact unit name."""
    return command(["/usr/bin/systemd-escape", "--path", "--suffix=mount", target]).stdout.strip()


def persistent(record: dict) -> bool:
    """Report an enabled mount unit or a matching current fstab entry."""
    if trusted(FSTAB) and any(item["uuid"] == record["uuid"] and item["target"] == record["target"] for item in fstab_records(FSTAB.read_text(), "Current fstab")):
        return True
    unit = unit_name(record["target"])
    path = UNITS / unit
    if not trusted(path):
        return False
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    settings.read(path)
    if settings.get("Mount", "What", fallback="") not in (f"/dev/disk/by-uuid/{record['uuid']}", f"UUID={record['uuid']}") or settings.get("Mount", "Where", fallback="") != record["target"] or settings.get("Mount", "Type", fallback="") != "ext4":
        return False
    return command(["/usr/bin/systemctl", "is-enabled", "--quiet", unit], check=False).returncode == 0


def snapshot() -> dict:
    """Describe only previously registered ext4 data volumes and their safe recovery eligibility."""
    mounts = mounted_filesystems()
    devices = discovered_devices()
    result = []
    for identifier, record in known_records(mounts).items():
        matches = [item for item in devices if (item.get("uuid") or "").lower() == identifier]
        device = matches[0] if len(matches) == 1 else None
        mounted = [item["target"] for item in mounts if (item.get("uuid") or "").lower() == identifier]
        configured = persistent(record)
        correct = mounted == [record["target"]]
        reason = "Ready to restore the saved mount." if not mounted else "Mounted at the saved location."
        allowed = device is not None and device.get("fstype") == "ext4" and (not mounted or correct)
        if not matches:
            reason = "Disk is disconnected or its UUID is not detected."
        elif len(matches) != 1:
            reason = "Duplicate UUIDs detected; automatic recovery is disabled."
        elif device.get("fstype") != "ext4":
            reason = "Filesystem type no longer matches the saved settings."
        elif mounted and not correct:
            reason = "Disk is mounted elsewhere; existing mounts will not be changed."
        elif device.get("ro") and not record.get("read_only"):
            allowed = False
            reason = "Disk is read-only; automatic recovery is disabled."
        result.append(record | {
            "device": device.get("path", "") if device else "", "label": device.get("label") or "" if device else "",
            "size_bytes": device.get("size", 0) if device else 0, "connected": device is not None,
            "mounted": correct, "mountpoints": mounted, "persistent": configured,
            "can_restore": allowed and not (correct and configured), "reason": reason,
        })
    return {"volumes": result, "supported_filesystems": ["ext4"], "saved_settings": trusted(STATE)}


def remember() -> dict:
    """Persist validated mappings outside /etc so later OS updates cannot erase the only copy."""
    records = known_records(mounted_filesystems())
    STATE.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".mounts-", dir=STATE.parent)
    with os.fdopen(descriptor, "w") as output:
        json.dump({"version": 1, "mounts": list(records.values())}, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    os.chmod(temporary, 0o644)
    os.replace(temporary, STATE)
    return {"success": True, "message": f"Saved mount settings for {len(records)} data volume(s)."}


def validate_target(target: Path, mounted: bool) -> None:
    """Refuse symlinked path components and refuse hiding existing files beneath a new mount."""
    for component in [target, *target.parents]:
        if component == Path("/mnt") and component.resolve() == Path("/var/mnt"):
            continue
        if component.is_symlink():
            raise ValueError("The saved mount path contains a symbolic link; automatic recovery was stopped.")
    if target.exists() and (not target.is_dir() or (not mounted and any(target.iterdir()))):
        raise ValueError("The saved mount location already contains files; they will not be hidden or overwritten.")


def restore(identifier: str) -> dict:
    """Restore one known ext4 volume by UUID after a non-writing check, never repairing or formatting."""
    if not UUID_PATTERN.fullmatch(identifier):
        raise ValueError("Invalid filesystem UUID.")
    record = next((item for item in snapshot()["volumes"] if item["uuid"] == identifier.lower()), None)
    if record is None:
        raise ValueError("No trusted saved mount settings exist for this UUID.")
    if record["mounted"] and record["persistent"]:
        return {"success": True, "message": "The saved mount is already active and persistent."}
    if not record["can_restore"]:
        raise ValueError(record["reason"])
    target = Path(record["target"])
    validate_target(target, record["mounted"])
    device = f"/dev/disk/by-uuid/{record['uuid']}"
    if not record["mounted"]:
        probe = json.loads(command(["/usr/bin/lsblk", "--json", "--output", "UUID,FSTYPE", device]).stdout)
        matching = probe["blockdevices"][0]
        if matching.get("uuid") != record["uuid"] or matching.get("fstype") != "ext4":
            raise ValueError("Device identity changed; automatic recovery was stopped.")
        check = command(["/usr/bin/e2fsck", "-fn", device], timeout=120, check=False)
        if check.returncode != 0:
            raise ValueError("The read-only filesystem check did not pass. No repair or mount was attempted. " + (check.stdout + check.stderr)[-2048:])
        if any((item.get("uuid") or "").lower() == identifier.lower() for item in mounted_filesystems()):
            raise ValueError("Another process mounted this disk during inspection. Refresh the page.")
        validate_target(target, False)
    unit = unit_name(record["target"])
    destination = UNITS / unit
    if destination.exists() or destination.is_symlink():
        if not trusted(destination):
            raise ValueError("An untrusted mount unit already exists; it will not be replaced.")
        settings = configparser.ConfigParser(interpolation=None, strict=False)
        settings.read(destination)
        if settings.get("Mount", "What", fallback="") not in (device, f"UUID={record['uuid']}") or settings.get("Mount", "Where", fallback="") != record["target"] or settings.get("Mount", "Type", fallback="") != "ext4":
            raise ValueError("An existing mount unit conflicts with the saved settings; it will not be replaced.")
    else:
        mode = "ro" if record.get("read_only") else "rw"
        contents = f"[Unit]\nDescription=SteamShine restored data storage\nJobTimeoutSec=30s\n\n[Mount]\nWhat={device}\nWhere={target}\nType=ext4\nOptions={mode},nosuid,nodev,errors=remount-ro\nTimeoutSec=30s\n\n[Install]\nWantedBy=local-fs.target\n"
        with destination.open("x") as output:
            output.write(contents)
        destination.chmod(0o644)
    command(["/usr/bin/systemctl", "daemon-reload"])
    command(["/usr/bin/systemctl", "enable", "--now", unit], timeout=45)
    verified = next(item for item in snapshot()["volumes"] if item["uuid"] == record["uuid"])
    if not verified["mounted"] or not verified["persistent"]:
        raise ValueError("Mount verification failed. Existing data was not modified.")
    remember()
    return {"success": True, "message": f"Restored {record['target']} and enabled it for future boots."}


def main() -> int:
    """Expose only read-only status and fixed authorized remember/restore operations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("authorize", "status", "remember", "restore"))
    parser.add_argument("uuid", nargs="?")
    arguments = parser.parse_args()
    try:
        if arguments.action != "status" and (os.geteuid() != 0 or not os.environ.get("SUDO_USER") or os.environ["SUDO_USER"] == "root"):
            raise ValueError("Storage changes require the installed privileged helper and an authenticated user.")
        if arguments.action != "restore" and arguments.uuid is not None:
            raise ValueError("Unexpected storage-helper argument.")
        if arguments.action == "authorize":
            return 0
        if arguments.action == "status":
            result = snapshot()
        else:
            STATE.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
            descriptor = os.open(STATE.parent / ".operation.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            with os.fdopen(descriptor, "w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = remember() if arguments.action == "remember" else restore(arguments.uuid or "")
        print(json.dumps(result))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired, configparser.Error) as error:
        print(json.dumps({"success": False, "message": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
