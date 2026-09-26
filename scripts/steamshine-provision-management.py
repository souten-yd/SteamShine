#!/usr/bin/python3
"""Install fixed-operation Web management helpers after sudo authenticates the local user."""

from __future__ import annotations

import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import tempfile


HELPERS = {
    "steamshine-runtime-helper": "steamshine-runtime-helper.sh",
    "steamshine-decky-helper": "steamshine-decky-helper.sh",
    "steamshine-storage-helper": "steamshine-storage-helper.py",
}
DESTINATION = Path("/var/lib/steamshine/helpers")
POLICY = Path("/etc/sudoers.d/steamshine-web-management")
KEEP = Path("/etc/atomic-update.conf.d/steamshine-web-management.conf")


def policy_for(username: str) -> str:
    """Grant only fixed helper operations to one validated local account."""
    if not re.fullmatch(r"[A-Za-z0-9_-]+", username) or username == "root":
        raise ValueError("A non-root local user is required.")
    runtime = str(DESTINATION / "steamshine-runtime-helper")
    decky = str(DESTINATION / "steamshine-decky-helper")
    storage = str(DESTINATION / "steamshine-storage-helper")
    operations = [f"{runtime} {action}" for action in ("authorize", "probe-gpu", "apply-profile *")]
    operations += [f"{decky} {action}" for action in ("authorize", "install", "update", "uninstall", "start", "configure-input")]
    operations += [f"{storage} {action}" for action in ("authorize", "status", "remember", "restore *")]
    return f"Cmnd_Alias STEAMSHINE_WEB_MANAGEMENT = {', '.join(operations)}\nDefaults!STEAMSHINE_WEB_MANAGEMENT !authenticate\n{username} ALL=(root) NOPASSWD: STEAMSHINE_WEB_MANAGEMENT\n"


def install_file(destination: Path, contents: bytes, mode: int) -> None:
    """Atomically install a root-owned file, preserving the previous version for recovery."""
    destination.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    if destination.is_symlink():
        raise ValueError(f"Refusing to replace a symbolic link: {destination}")
    if destination.exists() and destination.read_bytes() == contents:
        os.chown(destination, 0, 0)
        destination.chmod(mode)
        return
    descriptor, temporary = tempfile.mkstemp(prefix=f".{destination.name}-", dir=destination.parent)
    with os.fdopen(descriptor, "wb") as output:
        output.write(contents)
        output.flush()
        os.fsync(output.fileno())
    os.chmod(temporary, mode)
    if destination.exists():
        previous = destination.with_name(destination.name + ".previous")
        if previous.is_symlink():
            raise ValueError(f"Refusing a symbolic link at the backup path: {previous}")
        shutil.copy2(destination, previous)
    os.replace(temporary, destination)


def provision() -> None:
    """Provision helpers and update-preserved authorization, without applying GPU or Decky actions."""
    if os.geteuid() != 0:
        raise ValueError("Administrator authentication is required.")
    caller = os.environ.get("SUDO_USER", "")
    policy = policy_for(caller)
    if pwd.getpwnam(caller).pw_uid == 0:
        raise ValueError("A non-root local user is required.")
    source = Path(__file__).resolve().parent
    contents = {}
    for name, filename in HELPERS.items():
        path = source / filename
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise ValueError("The installed management package is incomplete.")
        contents[name] = path.read_bytes()
    with tempfile.TemporaryDirectory(prefix="steamshine-policy-", dir="/run") as directory:
        candidate = Path(directory) / "policy"
        candidate.write_text(policy)
        subprocess.run(["/usr/bin/visudo", "-cf", str(candidate)], check=True, stdout=subprocess.DEVNULL)
    for name, payload in contents.items():
        install_file(DESTINATION / name, payload, 0o755)
    install_file(POLICY, policy.encode(), 0o440)
    install_file(KEEP, (str(POLICY) + "\n").encode(), 0o644)
    # Saving a recovery copy of mount settings is best effort; failing to read
    # the current mounts must not block GPU and Decky authorization.
    remembered = subprocess.run([str(DESTINATION / "steamshine-storage-helper"), "remember"], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env={"PATH": "/usr/bin:/usr/sbin", "SUDO_USER": caller})
    if remembered.returncode != 0:
        print("Mount settings were not saved; they can be saved later from the Addons page.")


def main() -> int:
    """Consume no password or arbitrary operation; sudo handles authentication on its own stdin."""
    try:
        provision()
        print("Web management is ready. Administrator passwords are not saved.")
        return 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Management setup failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
