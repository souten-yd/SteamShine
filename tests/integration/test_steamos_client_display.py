#!/usr/bin/env python3
"""Tests for the KDE Moonlight client display-mode helper."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).parents[2] / "scripts" / "configure-steamos-client-display.py"
SPEC = importlib.util.spec_from_file_location("configure_steamos_client_display", SCRIPT)
assert SPEC and SPEC.loader
DISPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DISPLAY)


class ClientDisplayTests(unittest.TestCase):
    """Verify display selection, mode application, and exact restoration."""

    def setUp(self) -> None:
        """Create an isolated runtime and representative KScreen response."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.environment = {
            "XDG_RUNTIME_DIR": self.temporary.name,
            "SUNSHINE_CLIENT_WIDTH": "1920",
            "SUNSHINE_CLIENT_HEIGHT": "1080",
            "SUNSHINE_CLIENT_FPS": "60",
        }
        self.configuration = {
            "outputs": [
                {
                    "id": 1,
                    "name": "DP-1",
                    "connected": True,
                    "enabled": True,
                    "priority": 1,
                    "currentModeId": "1",
                    "scale": 1.5,
                    "modes": [
                        {"id": "1", "size": {"width": 3440, "height": 1440}, "refreshRate": 99.982},
                        {"id": "6", "size": {"width": 1920, "height": 1080}, "refreshRate": 60.0},
                        {"id": "50", "size": {"width": 1920, "height": 1080}, "refreshRate": 50.0},
                        {"id": "30", "size": {"width": 1920, "height": 1080}, "refreshRate": 100.0},
                    ],
                }
            ]
        }

    def test_selects_exact_size_and_nearest_refresh_rate(self) -> None:
        """Select the 60 Hz exact-size mode for a 60 FPS Moonlight request."""
        output = DISPLAY.select_output(self.configuration)
        mode = DISPLAY.select_mode(output, 1920, 1080, 60.0)
        self.assertEqual(mode["id"], "6")

    def test_rejects_invalid_request_and_missing_mode(self) -> None:
        """Reject malformed client dimensions and unsupported physical modes."""
        invalid = dict(self.environment, SUNSHINE_CLIENT_WIDTH="wide")
        with self.assertRaises(ValueError):
            DISPLAY.requested_mode(invalid)
        with self.assertRaises(ValueError):
            DISPLAY.select_mode(self.configuration["outputs"][0], 1170, 2532, 60.0)

    def test_selects_nearest_lower_refresh_and_rejects_higher_only_mode(self) -> None:
        """Allow a lower exact-size mode but never silently select a refresh above the request."""
        output = DISPLAY.select_output(self.configuration)
        self.assertEqual(DISPLAY.select_mode(output, 1920, 1080, 59.0)["id"], "50")
        with self.assertRaisesRegex(ValueError, "no safe"):
            DISPLAY.select_mode(output, 1920, 1080, 49.0)

    @mock.patch.object(DISPLAY, "read_configuration")
    def test_skips_host_display_changes_for_owned_virtual_session(self, read_configuration: mock.Mock) -> None:
        """Leave KScreen untouched when Moonlight already owns a sized virtual display."""
        virtual_environment = dict(self.environment, STEAMSHINE_VIRTUAL_DISPLAY_ORIGIN="owned_private")

        DISPLAY.apply_mode(virtual_environment)
        DISPLAY.revert_mode(virtual_environment)

        read_configuration.assert_not_called()
        self.assertFalse(DISPLAY.state_path(virtual_environment).exists())

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_refreshes_stale_service_graphical_environment(self, run: mock.Mock) -> None:
        """Use the current same-user Wayland socket instead of a stale service selector."""
        wayland_socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(wayland_socket.close)
        wayland_socket.bind(str(Path(self.temporary.name) / "wayland-current"))
        run.return_value = mock.Mock(
            stdout="WAYLAND_DISPLAY=wayland-current\nDISPLAY=:3\nUNRELATED_SECRET=ignored\n"
        )

        refreshed = DISPLAY.refresh_graphical_environment(
            dict(self.environment, WAYLAND_DISPLAY="wayland-stale", DISPLAY=":0")
        )

        self.assertEqual(refreshed["WAYLAND_DISPLAY"], "wayland-current")
        self.assertEqual(refreshed["DISPLAY"], ":3")
        self.assertEqual(refreshed["QT_QPA_PLATFORM"], "wayland")
        self.assertNotIn("UNRELATED_SECRET", refreshed)
        self.assertEqual(run.call_args.args[0], ["systemctl", "--user", "show-environment"])
        self.assertEqual(run.call_args.kwargs["timeout"], DISPLAY.GRAPHICAL_ENVIRONMENT_TIMEOUT_SECONDS)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_rejects_unvalidated_manager_graphical_environment(self, run: mock.Mock) -> None:
        """Reject absolute Wayland paths and non-local X display selectors."""
        run.return_value = mock.Mock(stdout="WAYLAND_DISPLAY=/tmp/foreign\nDISPLAY=host:0\n")

        with self.assertRaisesRegex(ValueError, "^No active graphical session display is available$"):
            DISPLAY.refresh_graphical_environment(self.environment)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_preserves_valid_inherited_selector_when_manager_value_is_invalid(self, run: mock.Mock) -> None:
        """Keep a verified service socket when the manager has no usable replacement."""
        wayland_socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(wayland_socket.close)
        wayland_socket.bind(str(Path(self.temporary.name) / "wayland-existing"))
        run.return_value = mock.Mock(stdout="WAYLAND_DISPLAY=/tmp/foreign\nDISPLAY=host:0\n")

        refreshed = DISPLAY.refresh_graphical_environment(
            dict(self.environment, WAYLAND_DISPLAY="wayland-existing")
        )

        self.assertEqual(refreshed["WAYLAND_DISPLAY"], "wayland-existing")
        self.assertNotIn("DISPLAY", refreshed)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_rejects_wayland_socket_symlink(self, run: mock.Mock) -> None:
        """Do not follow a manager-provided symlink even when its target is a socket."""
        wayland_socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(wayland_socket.close)
        target = Path(self.temporary.name) / "wayland-target"
        wayland_socket.bind(str(target))
        (Path(self.temporary.name) / "wayland-link").symlink_to(target)
        run.return_value = mock.Mock(stdout="WAYLAND_DISPLAY=wayland-link\n")

        with self.assertRaisesRegex(ValueError, "^No active graphical session display is available$"):
            DISPLAY.refresh_graphical_environment(self.environment)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_environment_query_timeout_is_stable(self, run: mock.Mock) -> None:
        """Bound the user-manager query and expose a stable timeout reason."""
        run.side_effect = subprocess.TimeoutExpired(["systemctl", "--user", "show-environment"], 5)

        with self.assertRaisesRegex(DISPLAY.DisplayCommandTimeout, "^kscreen_environment_timeout$"):
            DISPLAY.refresh_graphical_environment(self.environment)

    def test_environment_timeout_reports_virtual_fallback(self) -> None:
        """Map a bounded launch-time environment failure to the apply fallback result."""
        with (
            mock.patch.object(
                DISPLAY,
                "refresh_graphical_environment",
                side_effect=DISPLAY.DisplayCommandTimeout("environment"),
            ),
            mock.patch.object(DISPLAY.os, "environ", self.environment),
            mock.patch.object(DISPLAY.sys, "stderr") as stderr,
        ):
            result = DISPLAY.main([str(SCRIPT), "apply"])

        self.assertEqual(result, DISPLAY.VIRTUAL_FALLBACK_EXIT_CODE)
        self.assertIn("kscreen_environment_timeout", "".join(call.args[0] for call in stderr.write.call_args_list))

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_skips_environment_query_for_owned_virtual_session(self, run: mock.Mock) -> None:
        """Preserve the private compositor environment without consulting the host Desktop."""
        virtual_environment = dict(self.environment, STEAMSHINE_VIRTUAL_DISPLAY_ORIGIN="owned_private")

        self.assertEqual(DISPLAY.refresh_graphical_environment(virtual_environment), virtual_environment)
        run.assert_not_called()

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_applies_and_restores_original_mode_and_scale(self, run: mock.Mock) -> None:
        """Persist the original UWQHD state before applying and then restore it."""
        run.return_value = mock.Mock(stdout=json.dumps(self.configuration))

        DISPLAY.apply_mode(self.environment)

        state = DISPLAY.state_path(self.environment)
        self.assertEqual(json.loads(state.read_text(encoding="utf-8")), {"output": "DP-1", "mode_id": "1", "scale": 1.5})
        self.assertEqual(run.call_args_list[-1].args[0], ["kscreen-doctor", "output.DP-1.mode.6"])
        self.assertEqual(run.call_args_list[-1].kwargs["timeout"], DISPLAY.KSCREEN_APPLY_TIMEOUT_SECONDS)
        self.assertEqual(run.call_args_list[-1].kwargs["env"], self.environment)

        DISPLAY.revert_mode(self.environment)

        self.assertFalse(state.exists())
        self.assertEqual(
            run.call_args_list[-1].args[0],
            ["kscreen-doctor", "output.DP-1.mode.1", "output.DP-1.scale.1.5"],
        )
        self.assertEqual(run.call_args_list[-1].kwargs["timeout"], DISPLAY.KSCREEN_REVERT_TIMEOUT_SECONDS)
        self.assertEqual(run.call_args_list[-1].kwargs["env"], self.environment)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_uses_five_second_read_timeout(self, run: mock.Mock) -> None:
        """Bound KScreen configuration reads independently from apply and revert."""
        run.return_value = mock.Mock(stdout=json.dumps(self.configuration))

        DISPLAY.read_configuration(self.environment)

        self.assertEqual(run.call_args.kwargs["timeout"], DISPLAY.KSCREEN_READ_TIMEOUT_SECONDS)
        self.assertEqual(run.call_args.kwargs["env"], self.environment)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_converts_subprocess_timeout_to_stable_error(self, run: mock.Mock) -> None:
        """Hide variable subprocess timeout text behind a stable operation code."""
        run.side_effect = subprocess.TimeoutExpired(["kscreen-doctor", "-j"], 5)

        with self.assertRaisesRegex(DISPLAY.DisplayCommandTimeout, "^kscreen_read_timeout$"):
            DISPLAY.read_configuration(self.environment)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_apply_timeout_retains_state_and_reports_virtual_fallback(self, run: mock.Mock) -> None:
        """Keep restore state and return the virtual-fallback exit code after apply timeout."""
        run.side_effect = [
            mock.Mock(stdout=json.dumps(self.configuration)),
            subprocess.TimeoutExpired(["kscreen-doctor", "output.DP-1.mode.6"], 10),
        ]

        with (
            mock.patch.object(DISPLAY, "refresh_graphical_environment", return_value=self.environment),
            mock.patch.object(DISPLAY.os, "environ", self.environment),
            mock.patch.object(DISPLAY.sys, "stderr") as stderr,
        ):
            result = DISPLAY.main([str(SCRIPT), "apply"])

        self.assertEqual(result, DISPLAY.VIRTUAL_FALLBACK_EXIT_CODE)
        self.assertTrue(DISPLAY.state_path(self.environment).exists())
        self.assertIn("kscreen_apply_timeout", "".join(call.args[0] for call in stderr.write.call_args_list))

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_revert_timeout_retains_state_without_fallback(self, run: mock.Mock) -> None:
        """Retain recovery state and fail promptly when KScreen restore times out."""
        state = DISPLAY.state_path(self.environment)
        state.parent.mkdir(mode=0o700, parents=True)
        state.write_text(json.dumps({"output": "DP-1", "mode_id": "1", "scale": 1.5}), encoding="utf-8")
        run.side_effect = subprocess.TimeoutExpired(["kscreen-doctor"], 10)

        with (
            mock.patch.object(DISPLAY, "refresh_graphical_environment", return_value=self.environment),
            mock.patch.object(DISPLAY.os, "environ", self.environment),
            mock.patch.object(DISPLAY.sys, "stderr") as stderr,
        ):
            result = DISPLAY.main([str(SCRIPT), "revert"])

        self.assertEqual(result, 1)
        self.assertTrue(state.exists())
        output = "".join(call.args[0] for call in stderr.write.call_args_list)
        self.assertIn("kscreen_revert_timeout", output)
        self.assertIn("virtual_fallback_possible=false", output)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_refreshes_wayland_endpoint_from_systemd_user_manager(self, run: mock.Mock) -> None:
        """Use Plasma's endpoint when the resident service predates the desktop."""
        wayland_socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(wayland_socket.close)
        wayland_socket.bind(str(Path(self.temporary.name) / "wayland-0"))
        run.side_effect = [
            mock.Mock(
                stdout=(
                    "WAYLAND_DISPLAY=wayland-0\n"
                    "DISPLAY=:0\n"
                    "XDG_CURRENT_DESKTOP=KDE\n"
                    "XDG_SESSION_TYPE=wayland\n"
                )
            ),
            mock.Mock(stdout=json.dumps(self.configuration)),
            mock.Mock(stdout=""),
        ]

        DISPLAY.apply_mode(DISPLAY.current_desktop_environment(self.environment))

        self.assertEqual(run.call_args_list[0].args[0], ["systemctl", "--user", "show-environment"])
        kscreen_environment = run.call_args_list[1].kwargs["env"]
        self.assertEqual(kscreen_environment["WAYLAND_DISPLAY"], "wayland-0")
        self.assertEqual(kscreen_environment["DISPLAY"], ":0")
        self.assertEqual(kscreen_environment["SUNSHINE_CLIENT_WIDTH"], "1920")
        self.assertEqual(run.call_args_list[1].kwargs["timeout"], DISPLAY.KSCREEN_TIMEOUT_SECONDS)

    @mock.patch.object(DISPLAY.subprocess, "run")
    def test_ignores_non_kde_manager_endpoint(self, run: mock.Mock) -> None:
        """Do not route KScreen through a resident non-Plasma compositor."""
        run.return_value = mock.Mock(stdout="WAYLAND_DISPLAY=gamescope-0\nXDG_CURRENT_DESKTOP=gamescope\n")
        inherited = dict(self.environment, WAYLAND_DISPLAY="wayland-inherited")

        refreshed = DISPLAY.current_desktop_environment(inherited)

        self.assertEqual(refreshed["WAYLAND_DISPLAY"], "wayland-inherited")


if __name__ == "__main__":
    unittest.main()
