#!/usr/bin/env python3
"""Temporarily match a KDE physical output to the Moonlight video request."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from typing import Any


KSCREEN_READ_TIMEOUT_SECONDS = 5
KSCREEN_APPLY_TIMEOUT_SECONDS = 10
KSCREEN_REVERT_TIMEOUT_SECONDS = 10
GRAPHICAL_ENVIRONMENT_TIMEOUT_SECONDS = 5
VIRTUAL_FALLBACK_EXIT_CODE = 75


class DisplayCommandTimeout(RuntimeError):
    """Report a stable KScreen timeout without exposing subprocess details."""

    def __init__(self, operation: str) -> None:
        """Create a timeout for the named environment, read, apply, or revert operation."""
        super().__init__(f"kscreen_{operation}_timeout")


def run_kscreen(arguments: list[str], operation: str, timeout: int, **options: Any) -> subprocess.CompletedProcess[str]:
    """Run one bounded KScreen command and normalize timeout failures."""
    try:
        return subprocess.run(arguments, timeout=timeout, **options)
    except subprocess.TimeoutExpired as error:
        raise DisplayCommandTimeout(operation) from error


def state_path(environment: dict[str, str]) -> Path:
    """Return the per-user runtime path used to restore the physical display."""
    runtime = environment.get("XDG_RUNTIME_DIR", "")
    if not runtime or not Path(runtime).is_absolute():
        raise ValueError("XDG_RUNTIME_DIR must be an absolute path")
    return Path(runtime) / "steamshine" / "client-display-state.json"


def requested_mode(environment: dict[str, str]) -> tuple[int, int, float]:
    """Parse and validate the Moonlight-requested width, height, and frame rate."""
    try:
        width = int(environment["SUNSHINE_CLIENT_WIDTH"])
        height = int(environment["SUNSHINE_CLIENT_HEIGHT"])
        fps = float(environment["SUNSHINE_CLIENT_FPS"])
    except (KeyError, ValueError) as error:
        raise ValueError("Moonlight display request is unavailable") from error
    if not 640 <= width <= 7680 or not 480 <= height <= 4320 or not 1 <= fps <= 240:
        raise ValueError("Moonlight display request is outside supported bounds")
    return width, height, fps


def physical_display_selected(environment: dict[str, str]) -> bool:
    """Return whether SteamShine selected the host physical desktop."""
    return environment.get("STEAMSHINE_VIRTUAL_DISPLAY_ORIGIN", "none") == "none"


def parse_manager_environment(output: str) -> dict[str, str]:
    """Extract only graphical selectors from systemd user-manager output."""
    allowed = {"WAYLAND_DISPLAY", "DISPLAY"}
    parsed: dict[str, str] = {}
    for line in output.splitlines():
        name, separator, value = line.partition("=")
        if separator and name in allowed and "\x00" not in value:
            parsed[name] = value
    return parsed


def valid_wayland_display(environment: dict[str, str], value: str) -> bool:
    """Validate a same-user Wayland socket directly below XDG_RUNTIME_DIR."""
    if not value or value in {".", ".."} or Path(value).name != value:
        return False
    runtime = environment.get("XDG_RUNTIME_DIR", "")
    if not runtime or not Path(runtime).is_absolute():
        return False
    try:
        metadata = os.lstat(Path(runtime) / value)
    except OSError:
        return False
    return stat.S_ISSOCK(metadata.st_mode) and metadata.st_uid == os.getuid()


def valid_x11_display(value: str) -> bool:
    """Return whether a selector names a canonical local X display."""
    return re.fullmatch(r":[0-9]+(?:\.[0-9]+)?", value) is not None


def refresh_graphical_environment(environment: dict[str, str]) -> dict[str, str]:
    """Refresh stale service display selectors from the systemd user manager."""
    if not physical_display_selected(environment):
        return dict(environment)
    completed = run_kscreen(
        ["systemctl", "--user", "show-environment"],
        "environment",
        GRAPHICAL_ENVIRONMENT_TIMEOUT_SECONDS,
        check=True,
        capture_output=True,
        text=True,
    )
    manager = parse_manager_environment(completed.stdout)
    refreshed = dict(environment)
    wayland_display = manager.get("WAYLAND_DISPLAY", "")
    if not valid_wayland_display(refreshed, wayland_display):
        wayland_display = refreshed.get("WAYLAND_DISPLAY", "")
    if valid_wayland_display(refreshed, wayland_display):
        refreshed["WAYLAND_DISPLAY"] = wayland_display
        refreshed["QT_QPA_PLATFORM"] = "wayland"
    else:
        refreshed.pop("WAYLAND_DISPLAY", None)
        refreshed.pop("QT_QPA_PLATFORM", None)
    x11_display = manager.get("DISPLAY", "")
    if not valid_x11_display(x11_display):
        x11_display = refreshed.get("DISPLAY", "")
    if valid_x11_display(x11_display):
        refreshed["DISPLAY"] = x11_display
    else:
        refreshed.pop("DISPLAY", None)
    if "WAYLAND_DISPLAY" not in refreshed and "DISPLAY" not in refreshed:
        raise ValueError("No active graphical session display is available")
    return refreshed


def select_output(configuration: dict[str, Any], requested_name: str = "") -> dict[str, Any]:
    """Select the requested or primary connected and enabled KDE output."""
    outputs = [
        output
        for output in configuration.get("outputs", [])
        if output.get("connected") and output.get("enabled")
    ]
    if requested_name:
        outputs = [output for output in outputs if output.get("name") == requested_name]
    if not outputs:
        raise ValueError("No matching connected KDE output is enabled")
    return min(outputs, key=lambda output: (int(output.get("priority") or 9999), int(output.get("id") or 9999)))


def select_mode(output: dict[str, Any], width: int, height: int, fps: float) -> dict[str, Any]:
    """Select the exact requested size whose refresh rate is nearest the request."""
    modes = [
        mode
        for mode in output.get("modes", [])
        if int(mode.get("size", {}).get("width", 0)) == width
        and int(mode.get("size", {}).get("height", 0)) == height
    ]
    if not modes:
        raise ValueError(f"KDE output {output.get('name', 'unknown')} has no {width}x{height} mode")
    return min(modes, key=lambda mode: abs(float(mode.get("refreshRate", 0.0)) - fps))


def read_configuration(environment: dict[str, str]) -> dict[str, Any]:
    """Read the current KScreen configuration from kscreen-doctor."""
    completed = run_kscreen(
        ["kscreen-doctor", "-j"],
        "read",
        KSCREEN_READ_TIMEOUT_SECONDS,
        check=True,
        capture_output=True,
        env=environment,
        text=True,
    )
    return json.loads(completed.stdout)


def apply_mode(environment: dict[str, str]) -> None:
    """Save the current output state and apply the Moonlight-requested mode."""
    if not physical_display_selected(environment):
        return
    path = state_path(environment)
    if path.exists():
        revert_mode(environment)
    width, height, fps = requested_mode(environment)
    output = select_output(read_configuration(environment), environment.get("STEAMSHINE_DISPLAY_OUTPUT", ""))
    mode = select_mode(output, width, height, fps)
    saved_state = {
        "output": output["name"],
        "mode_id": str(output["currentModeId"]),
        "scale": float(output["scale"]),
    }
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(saved_state) + "\n", encoding="utf-8")
    temporary.chmod(0o600)
    temporary.replace(path)
    run_kscreen(
        ["kscreen-doctor", f"output.{output['name']}.mode.{mode['id']}"],
        "apply",
        KSCREEN_APPLY_TIMEOUT_SECONDS,
        check=True,
        env=environment,
    )


def revert_mode(environment: dict[str, str]) -> None:
    """Restore the physical output mode and scale saved before streaming."""
    if not physical_display_selected(environment):
        return
    path = state_path(environment)
    if not path.exists():
        return
    saved_state = json.loads(path.read_text(encoding="utf-8"))
    output = str(saved_state["output"])
    mode_id = str(saved_state["mode_id"])
    scale = float(saved_state["scale"])
    run_kscreen(
        [
            "kscreen-doctor",
            f"output.{output}.mode.{mode_id}",
            f"output.{output}.scale.{scale:g}",
        ],
        "revert",
        KSCREEN_REVERT_TIMEOUT_SECONDS,
        check=True,
        env=environment,
    )
    path.unlink()


def main(arguments: list[str]) -> int:
    """Apply or revert the client display mode selected on the command line."""
    if len(arguments) != 2 or arguments[1] not in {"apply", "revert"}:
        print(f"Usage: {arguments[0]} apply|revert", file=sys.stderr)
        return 2
    try:
        environment = refresh_graphical_environment(dict(os.environ))
        if arguments[1] == "apply":
            apply_mode(environment)
        else:
            revert_mode(environment)
    except (DisplayCommandTimeout, OSError, ValueError, KeyError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        fallback_possible = arguments[1] == "apply"
        print(
            "SteamShine client display configuration failed: "
            f"{error}; virtual_fallback_possible={str(fallback_possible).lower()}",
            file=sys.stderr,
        )
        return VIRTUAL_FALLBACK_EXIT_CODE if fallback_possible else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
