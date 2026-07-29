#!/usr/bin/env bash
# @file tests/integration/test_steamos_force_hardware_script.sh
# @brief Verify the force-mode hardware helper is safe until explicitly opted in.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
test_root="$(mktemp -d)"
trap 'rm -rf -- "${test_root}"' EXIT

# The guard executes before any service, config, or runtime access. This lets
# CI prove that invoking the helper accidentally cannot disturb a SteamOS host.
set +e
STEAMSHINE_FORCE_HARDWARE_TEST='' "${root_dir}/scripts/test-steamos-force-hardware.sh" \
  >"${test_root}/guard.log" 2>&1
status="$?"
set -e
if [[ "${status}" != 2 ]]; then
  echo "Expected the force hardware helper guard to exit 2, got ${status}." >&2
  exit 1
fi
grep -Fq 'STEAMSHINE_FORCE_HARDWARE_TEST=1' "${test_root}/guard.log"

# Keep the test configuration isolated: it must enable force mode in a copied
# configuration and never replace the user service unit's ExecStart.
grep -Fq 'steamos_virtual_display_mode = force' "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq 'steamos_keep_session_alive = false' "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq 'stop_owned_test_sessions' "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq 'stop_owned_test_surfaces' "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq "steamshine-steamos-virtual-session-v1" "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq 'temporary_config' "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq "\${runtime_root}/ss-fh.XXXXXX" "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq "kill -KILL \"\${test_pid}\"" "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq 'systemctl --user start steamshine.service' "${root_dir}/scripts/test-steamos-force-hardware.sh"
grep -Fq 'Web UI did not become ready within 30 seconds' "${root_dir}/scripts/test-steamos-force-hardware.sh"
