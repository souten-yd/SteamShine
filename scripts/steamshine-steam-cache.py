#!/usr/bin/env python3
"""Inspect Steam cache locations and configure internal shader storage without deleting data."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
import time


def parse_vdf(text: str) -> dict:
    """Parse the quoted KeyValues format used by Steam libraryfolders.vdf."""
    tokens = []
    pattern = re.compile(r'\s+|//[^\n]*|"((?:\\.|[^"\\])*)"|([{}])')
    offset = 0
    for match in pattern.finditer(text):
        if match.start() != offset:
            raise ValueError("Steam library list contains invalid KeyValues syntax.")
        offset = match.end()
        if match.group(1) is not None:
            tokens.append(("text", re.sub(r'\\([\\"])', r'\1', match.group(1))))
        elif match.group(2):
            tokens.append((match.group(2), match.group(2)))
    if offset != len(text):
        raise ValueError("Steam library list is incomplete.")
    position = 0

    def block(nested: bool = False, depth: int = 0) -> dict:
        """Read a balanced object while rejecting excessive nesting and truncation."""
        nonlocal position
        if depth > 16:
            raise ValueError("Steam library list is nested too deeply.")
        result = {}
        while position < len(tokens):
            kind, key = tokens[position]
            position += 1
            if kind == "}" and nested:
                return result
            if kind != "text" or position == len(tokens):
                raise ValueError("Steam library list is incomplete.")
            kind, value = tokens[position]
            position += 1
            if kind == "{":
                result[key] = block(True, depth + 1)
            elif kind == "text":
                result[key] = value
            else:
                raise ValueError("Steam library list contains an invalid value.")
        if nested:
            raise ValueError("Steam library list has an unclosed object.")
        return result

    return block()


def steam_root(home: Path) -> Path | None:
    """Find the current user's Steam installation without creating directories."""
    for candidate in (home / ".local/share/Steam", home / ".steam/steam"):
        if (candidate / "steamapps").is_dir():
            return candidate.resolve()
    return None


def libraries(root: Path) -> list[Path]:
    """Read registered libraries, preserving disconnected entries and deduplicating aliases."""
    paths = [root]
    listing = root / "steamapps/libraryfolders.vdf"
    if listing.exists():
        if listing.stat().st_size > 8 * 1024 * 1024:
            raise ValueError("Steam library list is too large.")
        document = parse_vdf(listing.read_text(encoding="utf-8"))
        entries = document.get("libraryfolders", document.get("LibraryFolders", {}))
        if not isinstance(entries, dict):
            raise ValueError("Steam library list is not an object.")
        for key, entry in entries.items():
            if not key.isdigit():
                continue
            value = entry.get("path") if isinstance(entry, dict) else entry
            if isinstance(value, str) and Path(value).is_absolute():
                paths.append(Path(value))
    return list(dict.fromkeys(path.resolve() for path in paths))


def library_id(path: Path) -> str:
    """Return an opaque identifier; API callers cannot supply arbitrary filesystem paths."""
    return hashlib.sha256(os.fsencode(path)).hexdigest()[:24]


def existing_parent(path: Path) -> Path:
    """Find a readable ancestor for storage-device and free-space checks."""
    while not path.exists() and path != path.parent:
        path = path.parent
    return path


def on_home_storage(path: Path, home: Path) -> bool:
    """Check actual device identity, following an existing cache link to its destination."""
    return existing_parent(path).stat().st_dev == home.stat().st_dev


def library_available(path: Path) -> bool:
    """Reject absent libraries and unmounted placeholders under common media directories."""
    if not (path / "steamapps").is_dir():
        return False
    for prefix in ("/run/media", "/media", "/mnt", "/var/mnt"):
        base = Path(prefix).resolve()
        if path.is_relative_to(base) and path.stat().st_dev == existing_parent(base).stat().st_dev:
            return False
    return True


def steam_active() -> bool:
    """Detect the user's Steam client, shader compiler, Wine processes, or Steam games."""
    names = {"steam", "steamwebhelper", "wineserver", "wine", "wine64", "fossilize_replay"}
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            if (process / "comm").read_text().strip() in names:
                return True
            environment = (process / "environ").read_bytes().split(b"\0")
            if any(value.startswith((b"SteamAppId=", b"SteamGameId=", b"STEAM_COMPAT_DATA_PATH=")) for value in environment):
                return True
        except (OSError, UnicodeError):
            continue
    return False


def cache_status(path: Path, home: Path, available: bool, active: bool) -> dict:
    """Describe an existing cache or Proton directory without moving or traversing its contents."""
    result = {"path": str(path), "target": str(path.resolve()), "configured": False, "can_configure": False}
    if not available:
        return result | {"state": "unavailable", "reason": "Connect and mount this library first."}
    if path.is_symlink() and not path.is_dir():
        return result | {"state": "broken_link", "reason": "The existing link is unavailable; it will not be replaced."}
    if path.exists() and not path.is_dir():
        return result | {"state": "invalid", "reason": "This path is not a directory; it will not be replaced."}
    if on_home_storage(path, home):
        return result | {"state": "internal", "configured": True, "reason": "Already using internal storage."}
    if path.is_symlink():
        return result | {"state": "custom_link", "reason": "An existing custom link is preserved."}
    writable = os.access(path.parent, os.W_OK)
    reason = "Exit Steam and running games before changing the cache location." if active else (
        "The library is read-only." if not writable else "Shader cache is stored with this game library."
    )
    return result | {"state": "library", "can_configure": not active and writable, "reason": reason}


def snapshot(home: Path) -> dict:
    """Return cache locations, connection state, and internal free space for the Addon page."""
    root = steam_root(home)
    active = steam_active()
    result = {"available": root is not None, "steam_running": active, "home_free_bytes": shutil.disk_usage(home).free, "libraries": []}
    if root is None:
        return result | {"message": "Steam is not installed for this user."}
    result["steam_root"] = str(root)
    for path in libraries(root):
        available = library_available(path)
        result["libraries"].append({
            "id": library_id(path), "path": str(path), "available": available,
            "shadercache": cache_status(path / "steamapps/shadercache", home, available, active),
            "compatdata": cache_status(path / "steamapps/compatdata", home, available, True),
        })
    return result


def cache_bytes(source: Path) -> int:
    """Count copy space and reject links or special files rather than following them."""
    total = 0
    if not source.exists():
        return total
    for directory, directories, files in os.walk(source):
        for name in directories + files:
            entry = Path(directory) / name
            mode = entry.lstat().st_mode
            if stat.S_ISREG(mode):
                total += entry.stat().st_size
            elif not stat.S_ISDIR(mode):
                raise ValueError("The cache contains links or special files; automatic migration was stopped.")
    return total


def configure(home: Path, identifier: str) -> dict:
    """Copy one registered shader cache to internal storage, retaining the original as a backup.

    No Proton data, saves, Steam preferences, unrelated links, or disconnected library paths
    are modified. A failed copy leaves the original untouched. A failed link installation
    restores the original directory when the path is still free.
    """
    if not re.fullmatch(r"[0-9a-f]{24}", identifier):
        raise ValueError("Invalid Steam library identifier.")
    state = snapshot(home)
    library = next((item for item in state["libraries"] if item["id"] == identifier), None)
    if library is None:
        raise ValueError("This Steam library is no longer registered. Refresh the page.")
    status = library["shadercache"]
    if status["configured"]:
        return {"success": True, "message": "Shader cache already uses internal storage.", "changed": False}
    if not status["can_configure"]:
        raise ValueError(status["reason"])
    root = Path(state["steam_root"])
    storage = root / "InternalCache/SteamShine"
    if not on_home_storage(storage, home):
        raise ValueError("Steam's internal cache directory is on another storage device.")
    storage.mkdir(parents=True, exist_ok=True)
    lock_path = storage / ".configure.lock"
    descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("Another cache configuration is in progress.") from error
        source = Path(status["path"])
        if steam_active():
            raise ValueError("Exit Steam and running games before changing the cache location.")
        if not library_available(Path(library["path"])) or source.is_symlink():
            raise ValueError("Library state changed. Refresh the page before trying again.")
        original_identity = source.stat() if source.exists() else None
        required = cache_bytes(source)
        if shutil.disk_usage(storage).free < required + 1024 ** 3:
            raise ValueError("Internal storage needs room for the cache plus 1 GiB free space.")
        copy_root = Path(tempfile.mkdtemp(prefix=f"{identifier}-", dir=storage))
        destination = copy_root / "shadercache"
        try:
            if original_identity is None:
                destination.mkdir()
            else:
                shutil.copytree(source, destination, symlinks=True)
            if steam_active():
                raise ValueError("Steam started during the copy. Original data is unchanged.")
            if not library_available(Path(library["path"])):
                raise ValueError("The library disconnected during the copy. Original data was not moved.")
            current_identity = source.stat() if source.exists() else None
            if (original_identity is None) != (current_identity is None) or (
                original_identity is not None and (
                    source.is_symlink() or original_identity.st_ino != current_identity.st_ino
                    or original_identity.st_dev != current_identity.st_dev
                )
            ):
                raise ValueError("The cache path changed during the copy. Original data was not moved.")
            backup = source.with_name(f"shadercache.steamshine-backup-{time.time_ns()}")
            if os.path.lexists(backup):
                raise ValueError("A backup already exists at the new backup path; nothing was replaced.")
        except BaseException:
            # Only the private copy created above is removed; the original is untouched.
            shutil.rmtree(copy_root, ignore_errors=True)
            raise
        if original_identity is not None:
            source.rename(backup)
        try:
            source.symlink_to(destination, target_is_directory=True)
        except OSError:
            if original_identity is not None and not os.path.lexists(source):
                backup.rename(source)
            raise
        return {
            "success": True, "changed": True, "target": str(destination),
            "backup_path": str(backup) if original_identity is not None else "",
            "message": "Shader cache now uses internal storage. Original data was retained; Proton data was not changed.",
        }


def main() -> int:
    """Handle fixed status/configure operations and emit one JSON result."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", "configure"))
    parser.add_argument("library_id", nargs="?")
    arguments = parser.parse_args()
    try:
        result = snapshot(Path.home()) if arguments.action == "status" else configure(Path.home(), arguments.library_id or "")
        print(json.dumps(result))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"success": False, "message": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
