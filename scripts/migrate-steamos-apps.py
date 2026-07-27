#!/usr/bin/env python3
"""Add SteamShine display preparation to existing Sunshine applications."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Any


DISPLAY_HOOK = {
    "do": "$(HOME)/.local/share/steamshine/current/scripts/configure-steamos-client-display.py apply",
    "undo": "$(HOME)/.local/share/steamshine/current/scripts/configure-steamos-client-display.py revert",
}
TARGET_APPLICATIONS = {"Desktop", "Steam Big Picture"}


def add_display_hook(application: dict[str, Any]) -> bool:
    """Insert the display hook into one target application when absent.

    Args:
        application: Sunshine application object to inspect and update.

    Returns:
        ``True`` when the application was changed.

    Raises:
        ValueError: If a target application has an invalid preparation list.
    """
    if application.get("name") not in TARGET_APPLICATIONS:
        return False
    commands = application.setdefault("prep-cmd", [])
    if not isinstance(commands, list):
        raise ValueError(f"Application {application.get('name')} has an invalid prep-cmd value")
    if DISPLAY_HOOK in commands:
        return False
    commands.insert(0, dict(DISPLAY_HOOK))
    return True


def migrate(apps_path: Path) -> bool:
    """Migrate an applications file atomically while preserving a backup.

    Args:
        apps_path: Path to the Sunshine applications JSON file.

    Returns:
        ``True`` when the file was changed.

    Raises:
        ValueError: If the applications document has an invalid structure.
        OSError: If the file cannot be read, backed up, or replaced.
    """
    if not apps_path.exists():
        return False
    payload = json.loads(apps_path.read_text(encoding="utf-8"))
    applications = payload.get("apps") if isinstance(payload, dict) else None
    if not isinstance(applications, list):
        raise ValueError("Applications JSON must contain an apps array")
    changed = False
    for application in applications:
        if not isinstance(application, dict):
            raise ValueError("Applications JSON contains a non-object entry")
        changed = add_display_hook(application) or changed
    if not changed:
        return False

    backup = apps_path.with_name(apps_path.name + ".steamshine-backup")
    if not backup.exists():
        shutil.copy2(apps_path, backup)
    mode = apps_path.stat().st_mode & 0o777
    apps_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=apps_path.name + ".", dir=apps_path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(payload, output, indent=2, ensure_ascii=False)
            output.write("\n")
        temporary.chmod(mode)
        temporary.replace(apps_path)
    finally:
        temporary.unlink(missing_ok=True)
    return True


def main() -> int:
    """Migrate the applications file passed on the command line.

    Returns:
        The process exit status.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apps_path", type=Path)
    arguments = parser.parse_args()
    try:
        migrate(arguments.apps_path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"SteamShine application migration failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
