#!/usr/bin/env bash
# @file tests/integration/test_steamshine_script.sh
# @brief Smoke tests for the SteamShine lifecycle command argument boundary.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
test_root="$(mktemp -d)"
fake_dri="$(mktemp -d)"
trap 'rm -rf -- "${test_root}" "${fake_dri}"' EXIT
# shellcheck source=tests/fixtures/steamos/fixture.sh
source "${root_dir}/tests/fixtures/steamos/fixture.sh"
steamos_fixture_init "${test_root}/fixture"

# The CI timing report must split compiler work from the final runtime link
# without requiring a Sunshine build in shell-only validation.
python3 "${root_dir}/scripts/collect-ninja-timing.py" "${root_dir}/tests/fixtures/steamos/ninja.log" "${test_root}/ninja-timings.json"
grep -Fq '"tasks": 2' "${test_root}/ninja-timings.json"
grep -Fq '"milliseconds": 300' "${test_root}/ninja-timings.json"
grep -Fq '"milliseconds": 50' "${test_root}/ninja-timings.json"

# Workflow timing uploads are JSON arrays. Verify the comparison command uses
# that actual artifact shape rather than a hypothetical wrapper object.
cat >"${test_root}/timing-baseline.json" <<'EOF'
[{"name":"full-validation","started_at":"2026-01-01T00:00:00Z","completed_at":"2026-01-01T00:00:02Z","steps":[{"name":"Build runtime binary","seconds":10}]}]
EOF
cat >"${test_root}/timing-candidate.json" <<'EOF'
[{"name":"full-validation","started_at":"2026-01-02T00:00:00Z","completed_at":"2026-01-02T00:00:03Z","steps":[{"name":"Build runtime binary","seconds":8}]}]
EOF
bash "${root_dir}/scripts/compare-steamos-ci-timings.sh" "${test_root}/timing-baseline.json" -- "${test_root}/timing-candidate.json" >"${test_root}/timing-comparison.tsv"
grep -Fq $'Build runtime binary\t10.00\t8.00\t-2.00\t-20.0%' "${test_root}/timing-comparison.tsv"

# Only numeric GLIBCXX symbol versions are ABI candidates.  libstdc++ also
# exposes GLIBCXX_TUNABLES, which must never be selected as a version ceiling.
runtime_baseline="$("${root_dir}/scripts/collect-steamos-runtime-baseline.sh")"
if grep -Fq '"max_glibcxx": "GLIBCXX_TUNABLES"' <<<"${runtime_baseline}"; then
  echo 'Runtime baseline selected GLIBCXX_TUNABLES instead of a numeric ABI version.' >&2
  exit 1
fi

# VA-API is optional, but the compatibility report must identify the AMD driver
# by its actual radeonsi filename and never mistake an Intel i965 driver for it.
touch "${fake_dri}/i965_drv_video.so"
vaapi_output="$(STEAMSHINE_DRI_ROOTS="${fake_dri}" "${root_dir}/steamshine.sh" vaapi-driver-status 2>&1)"
if grep -Fq 'VAAPI_AMD_DRIVER_AVAILABLE' <<<"${vaapi_output}"; then
  echo 'An Intel-only VA-API directory was misidentified as AMD radeonsi.' >&2
  exit 1
fi
touch "${fake_dri}/radeonsi_drv_video.so"
vaapi_output="$(STEAMSHINE_DRI_ROOTS="${fake_dri}" "${root_dir}/steamshine.sh" vaapi-driver-status 2>&1)"
grep -Fq 'VAAPI_AMD_DRIVER_AVAILABLE' <<<"${vaapi_output}"

"${root_dir}/steamshine.sh" --help >/dev/null
if "${root_dir}/steamshine.sh" </dev/null >/dev/null 2>&1; then
  echo 'Expected non-TTY invocation without a command to fail.' >&2
  exit 1
fi
if "${root_dir}/steamshine.sh" uninstall --purge --non-interactive --dry-run >/dev/null 2>&1; then
  echo 'Expected non-interactive purge without --yes to fail.' >&2
  exit 1
fi

# A normal tar archive contains a leading ./ entry.  It is safe and must not be
# mistaken for a parent-directory traversal by the immutable artifact installer.
mkdir -p "${test_root}/stage/bin" "${test_root}/home/run"
install -m 755 /bin/true "${test_root}/stage/bin/steamshine"
printf '{"target_architecture":"x86_64"}\n' >"${test_root}/stage/BUILD_INFO.json"
printf '{}\n' >"${test_root}/stage/STEAMOS_BASELINE.json"
tar --zstd -C "${test_root}/stage" -cf "${test_root}/steamshine-steamos-x86_64-test.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-x86_64-test.tar.zst >steamshine-steamos-x86_64-test.tar.zst.sha256)
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-service --non-interactive --yes

# PR installation must ignore a newer docs-only success run and select the
# newest successful run that actually contains the immutable delivery archive.
# It must also not substitute a stale archive found elsewhere in the cache.
mkdir -p "${test_root}/mock-bin" "${test_root}/pr-home/run"
cat >"${test_root}/mock-bin/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  pr)
    printf '%s\n' 'feature/virtual-display'
    ;;
  run)
    if [[ "${2:-}" == list ]]; then
      # 900 is a later docs-only run; 800 is the latest full delivery run.
      printf '%s\n' 900 800
    elif [[ "${2:-}" == download ]]; then
      output_dir=''
      while (($#)); do
        if [[ "$1" == --dir ]]; then output_dir="$2"; shift 2; continue; fi
        shift
      done
      mkdir -p "${output_dir}/steamshine-steamos-x86_64-deadbeef"
      cp "${FIXTURE_ARTIFACT}" "${output_dir}/steamshine-steamos-x86_64-deadbeef/steamshine-steamos-x86_64-deadbeef.tar.zst"
      sed 's/steamshine-steamos-x86_64-test.tar.zst/steamshine-steamos-x86_64-deadbeef.tar.zst/' "${FIXTURE_ARTIFACT}.sha256" >"${output_dir}/steamshine-steamos-x86_64-deadbeef/steamshine-steamos-x86_64-deadbeef.tar.zst.sha256"
    fi
    ;;
  api)
    if [[ "$2" == *'/900/artifacts' ]]; then
      printf '%s\n' 'steamos-ci-timings-docs-only'
    elif [[ "$2" == *'/800/artifacts' ]]; then
      printf '%s\n' 'steamshine-steamos-x86_64-deadbeef'
    fi
    ;;
esac
EOF
chmod 755 "${test_root}/mock-bin/gh"
mkdir -p "${test_root}/pr-home/.cache/steamshine/artifacts/stale"
cp "${test_root}/steamshine-steamos-x86_64-test.tar.zst" "${test_root}/pr-home/.cache/steamshine/artifacts/stale/steamshine-steamos-x86_64-stale.tar.zst"
FIXTURE_ARTIFACT="${test_root}/steamshine-steamos-x86_64-test.tar.zst" HOME="${test_root}/pr-home" XDG_RUNTIME_DIR="${test_root}/pr-home/run" PATH="${test_root}/mock-bin:${PATH}" \
  "${root_dir}/steamshine.sh" install --channel pr --pr 6 --no-service --non-interactive --yes
test -x "${test_root}/pr-home/.local/bin/steamshine"
test -f "${test_root}/pr-home/.cache/steamshine/artifacts/800/steamshine-steamos-x86_64-deadbeef/steamshine-steamos-x86_64-deadbeef.tar.zst"

# The service passes Sunshine's configuration file as its positional argument.
# `--config` is not a Sunshine CLI option and would otherwise cause a restart
# loop before the host begins accepting Moonlight connections.
mkdir -p "${test_root}/mock-bin"
cat >"${test_root}/mock-bin/systemctl" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod 755 "${test_root}/mock-bin/systemctl"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --non-interactive --yes
service_unit="${test_root}/home/.config/systemd/user/steamshine.service"
grep -Fq "ExecStart=${test_root}/home/.local/bin/steamshine ${test_root}/home/.config/steamshine/sunshine.conf" "${service_unit}"
if grep -Fq -- '--config' "${service_unit}"; then
  echo 'The generated service must not pass an unsupported --config option.' >&2
  exit 1
fi
test -x "${test_root}/home/.local/bin/steamshine"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-service --non-interactive --yes

# A detached checksum is mandatory and a mismatch must not reach extraction.
cp "${test_root}/steamshine-steamos-x86_64-test.tar.zst" "${test_root}/steamshine-steamos-x86_64-corrupt.tar.zst"
printf '0000000000000000000000000000000000000000000000000000000000000000  steamshine-steamos-x86_64-corrupt.tar.zst\n' >"${test_root}/steamshine-steamos-x86_64-corrupt.tar.zst.sha256"
if HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-corrupt.tar.zst" --no-service --non-interactive --yes >/dev/null 2>&1; then
  echo 'Expected checksum mismatch to be rejected.' >&2
  exit 1
fi

# Artifact links are rejected before extraction, preventing link traversal in
# the version store even when the archive otherwise has the required layout.
mkdir -p "${test_root}/linked-stage/bin"
install -m 755 /bin/true "${test_root}/linked-stage/bin/steamshine"
printf '{"target_architecture":"x86_64"}\n' >"${test_root}/linked-stage/BUILD_INFO.json"
printf '{}\n' >"${test_root}/linked-stage/STEAMOS_BASELINE.json"
ln -s /etc/passwd "${test_root}/linked-stage/unsafe-link"
tar --zstd -C "${test_root}/linked-stage" -cf "${test_root}/steamshine-steamos-x86_64-linked.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-x86_64-linked.tar.zst >steamshine-steamos-x86_64-linked.tar.zst.sha256)
if HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-linked.tar.zst" --no-service --non-interactive --yes >/dev/null 2>&1; then
  echo 'Expected symlink-containing artifact to be rejected.' >&2
  exit 1
fi

# Metadata architecture is checked after extraction before the version switch.
mkdir -p "${test_root}/wrong-arch-stage/bin"
install -m 755 /bin/true "${test_root}/wrong-arch-stage/bin/steamshine"
printf '{"target_architecture":"aarch64"}\n' >"${test_root}/wrong-arch-stage/BUILD_INFO.json"
printf '{}\n' >"${test_root}/wrong-arch-stage/STEAMOS_BASELINE.json"
tar --zstd -C "${test_root}/wrong-arch-stage" -cf "${test_root}/steamshine-steamos-aarch64-test.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-aarch64-test.tar.zst >steamshine-steamos-aarch64-test.tar.zst.sha256)
if HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-aarch64-test.tar.zst" --no-service --non-interactive --yes >/dev/null 2>&1; then
  echo 'Expected non-x86_64 artifact metadata to be rejected.' >&2
  exit 1
fi

# A second validated version records the first version as the rollback target.
printf 'second version\n' >"${test_root}/stage/version-marker"
tar --zstd -C "${test_root}/stage" -cf "${test_root}/steamshine-steamos-x86_64-second.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-x86_64-second.tar.zst >steamshine-steamos-x86_64-second.tar.zst.sha256)
first_version="$(sha256sum "${test_root}/steamshine-steamos-x86_64-test.tar.zst" | awk '{print $1}')"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-second.tar.zst" --no-service --non-interactive --yes
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" rollback --no-service --non-interactive --yes
test "$(readlink -f "${test_root}/home/.local/share/steamshine/current")" = "${test_root}/home/.local/share/steamshine/versions/${first_version}"

# Immutable SteamOS installs must be removable without local development tools.
# The normal uninstall removes only generated binaries/cache/runtime files and
# deliberately preserves user configuration and diagnostic state.
mkdir -p "${test_root}/home/.config/steamshine" "${test_root}/home/.local/state/steamshine"
printf 'keep\n' >"${test_root}/home/.config/steamshine/sunshine.conf"
printf 'keep\n' >"${test_root}/home/.local/state/steamshine/diagnostics.log"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" uninstall --build-dir "${test_root}/cmake-build-steamos" --no-service --non-interactive --yes
test ! -e "${test_root}/home/.local/bin/steamshine"
test ! -e "${test_root}/home/.local/share/steamshine/current"
test ! -d "${test_root}/home/.cache/steamshine"
test -f "${test_root}/home/.config/steamshine/sunshine.conf"
test -f "${test_root}/home/.local/state/steamshine/diagnostics.log"

# Hardware-test helper scripts must tolerate SteamOS installations without
# pidstat or vainfo and must sum multiple process I/O counters safely.
proc_root="${PROC_ROOT}"
steamos_fixture_write_proc_io 101 28672 4
steamos_fixture_write_proc_io 202 4096 2
printf '101 (steamshine) S 0 0 0 0 0 0 0 0 0 0 10 5\n' >"${proc_root}/101/stat"
printf 'VmRSS:\t4096 kB\nvoluntary_ctxt_switches:\t2\nnonvoluntary_ctxt_switches:\t1\n' >"${proc_root}/101/status"
mkdir -p "${test_root}/hardware-bin"
cat >"${test_root}/hardware-bin/pgrep" <<'EOF'
#!/usr/bin/env bash
# Include a vanished PID without an io file; collection must skip it safely.
printf '101\n202\n303\n'
EOF
chmod 755 "${test_root}/hardware-bin/pgrep"
cat >"${test_root}/hardware-bin/journalctl" <<'EOF'
#!/usr/bin/env bash
# A completed owned session must leave all three event classes in the user
# journal.  Repeating this bounded fixture models each acceptance cycle.
printf '%s\n' 'SteamOS virtual display capture attached'
printf '%s\n' 'SteamOS virtual display streaming started'
printf '%s\n' 'SteamOS virtual display stopping owned Gamescope session'
printf '%s\n' 'SteamOS virtual display encoded packets=42 bytes=8192 idr=1 captured_frames=60'
EOF
chmod 755 "${test_root}/hardware-bin/journalctl"
cat >"${test_root}/hardware-bin/gamescope" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == '--version' ]]; then
  printf '%s\n' 'gamescope 3.16.23.4-test'
elif [[ "${1:-}" == '--help' ]]; then
  printf '%s\n' '--backend headless --nested-width --nested-height --nested-refresh --expose-wayland --prefer-vk-device'
fi
EOF
chmod 755 "${test_root}/hardware-bin/gamescope"
cat >"${test_root}/hardware-bin/vulkaninfo" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' 'Vulkan test fixture: AMD discrete GPU'
EOF
chmod 755 "${test_root}/hardware-bin/vulkaninfo"
hardware_report="${test_root}/hardware-report"
PATH="${test_root}/hardware-bin:${PATH}" PROC_ROOT="${proc_root}" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_report}" \
  "${root_dir}/scripts/test-steamos-ssd-writes.sh" 0
grep -Fq 'write_bytes=32768' "${hardware_report}/ssd-writes.log"
grep -Fq 'delta write_bytes=0' "${hardware_report}/ssd-writes.log"
grep -Fq 'journal_bytes=' "${hardware_report}/ssd-writes.log"
PATH="${test_root}/hardware-bin:${PATH}" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_report}" \
  PROC_ROOT="${proc_root}" "${root_dir}/scripts/test-steamos-latency.sh" 0
grep -Fq 'pidstat unavailable' "${hardware_report}/latency.log"
grep -Fq 'proc_cpu_delta pid=101 cpu_ticks=0' "${hardware_report}/latency.log"

# The interactive harness must preserve evidence even where the SteamOS image
# omits optional diagnostic programs.  The fake binary accepts the encoder
# preflight, while zero sampling duration keeps this lifecycle test fast.
cat >"${test_root}/hardware-bin/steamshine" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod 755 "${test_root}/hardware-bin/steamshine"
hardware_acceptance_report="${test_root}/hardware-acceptance-report"
acceptance_input="${test_root}/acceptance-input"
cat >"${test_root}/hardware-bin/event-hook" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
event="$1"
runtime_dir="$2"
proc_root="$3"
session="${runtime_dir}/session-fixture"
if [[ "${event}" == connected-* ]]; then
  mkdir -p "${session}" "${proc_root}/101"
  printf '%s\n' 'steamshine-steamos-virtual-session-v1' >"${session}/steamshine-owner"
  python3 - "${session}/gamescope-0" <<'PY' &
import signal
import socket
import sys
path = sys.argv[1]
sock = socket.socket(socket.AF_UNIX)
sock.bind(path)
sock.listen()
signal.pause()
PY
  echo "$!" >"${session}/socket.pid"
  for _ in $(seq 1 20); do [[ -S "${session}/gamescope-0" ]] && break; sleep 0.01; done
  printf 'XDG_RUNTIME_DIR=%s\0' "${session}" >"${proc_root}/101/environ"
else
  if [[ -r "${session}/socket.pid" ]]; then kill "$(<"${session}/socket.pid")" 2>/dev/null || true; fi
  rm -rf -- "${session}" "${proc_root}/101"
fi
EOF
chmod 755 "${test_root}/hardware-bin/event-hook"
for _ in $(seq 1 20); do printf '\n'; done >"${acceptance_input}"
for _ in video audio keyboard mouse gamepad; do printf 'y\n'; done >>"${acceptance_input}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/hardware-bin:${PATH}" \
  STEAMSHINE_BINARY="${test_root}/hardware-bin/steamshine" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_acceptance_report}" \
  STEAMSHINE_HARDWARE_SAMPLE_SECONDS=0 STEAMSHINE_TEST_MODE=1 STEAMSHINE_TEST_EVENT_HOOK="${test_root}/hardware-bin/event-hook" \
  "${root_dir}/scripts/test-steamos-virtual-display.sh" <"${acceptance_input}"
grep -Fq '"result": "pass"' "${hardware_acceptance_report}/hardware-report.json"
grep -Fq '"connect_disconnect_cycles": 10' "${hardware_acceptance_report}/hardware-report.json"
test "$(wc -l <"${hardware_acceptance_report}/encoded-stream-evidence.tsv")" -eq 10
grep -Fq '"captured_frame_count": 60' "${hardware_acceptance_report}/hardware-report.json"
test "$(grep -c '^owned_session_evidence=connected-' "${hardware_acceptance_report}/virtual-display.log")" -eq 10
