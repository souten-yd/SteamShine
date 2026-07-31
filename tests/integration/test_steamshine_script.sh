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

# Every deployable SteamOS workflow job must use the immutable image recorded
# in the repository lock rather than an independently updated digest.
locked_image="$(sed -n 's/^image=//p' "${root_dir}/ci/steamos/image.lock")"
locked_digest="$(sed -n 's/^digest=//p' "${root_dir}/ci/steamos/image.lock")"
mapfile -t workflow_images < <(sed -n 's/^[[:space:]]*image: //p' "${root_dir}/.github/workflows/build-steamos.yml")
test -n "${locked_image}" && test -n "${locked_digest}" && test "${#workflow_images[@]}" -gt 0
for workflow_image in "${workflow_images[@]}"; do
  test "${workflow_image}" = "${locked_image}@${locked_digest}"
done

# The Game Mode guard must ignore writeback connectors, hold the vendor launcher
# while headless, and resume the unmodified launcher when a physical connector
# becomes connected.
guard_drm="${test_root}/guard-drm"
guard_marker="${test_root}/guard-launcher-called"
guard_runtime="${test_root}/guard-runtime"
guard_lease="${guard_runtime}/steamshine/stock-session-handoff.lease"
mkdir -p "${guard_drm}/card0-Writeback-1" "${guard_drm}/card0-HDMI-A-1" "$(dirname "${guard_lease}")"
printf 'connected\n' >"${guard_drm}/card0-Writeback-1/status"
printf 'disconnected\n' >"${guard_drm}/card0-HDMI-A-1/status"
cat >"${test_root}/vendor-gamescope-session" <<'EOF'
#!/usr/bin/env bash
printf 'called\n' >"${STEAMSHINE_GUARD_TEST_MARKER:?}"
EOF
chmod 755 "${test_root}/vendor-gamescope-session"
STEAMSHINE_DRM_ROOT="${guard_drm}" \
  STEAMSHINE_CONNECTOR_POLL_SECONDS=0.01 \
  STEAMSHINE_GUARD_TEST_MARKER="${guard_marker}" \
  XDG_RUNTIME_DIR="${guard_runtime}" \
  "${root_dir}/scripts/steamshine-gamescope-session-guard.sh" "${test_root}/vendor-gamescope-session" &
guard_pid=$!
sleep 0.05
test ! -e "${guard_marker}"
printf 'connected\n' >"${guard_drm}/card0-HDMI-A-1/status"
wait "${guard_pid}"
grep -Fxq 'called' "${guard_marker}"

# A live owner lease also holds a connected stock launcher, then releasing the
# lease resumes it exactly once. A stale owner must be removed automatically.
rm -f "${guard_marker}"
guard_boot_id="$(< /proc/sys/kernel/random/boot_id)"
guard_owner_start="$(awk '{print $22}' "/proc/$$/stat")"
printf 'version=1\nboot_id=%s\nowner_pid=%s\nowner_start_time=%s\ngeneration=1\n' \
  "${guard_boot_id}" "$$" "${guard_owner_start}" >"${guard_lease}"
chmod 600 "${guard_lease}"
STEAMSHINE_DRM_ROOT="${guard_drm}" \
  STEAMSHINE_CONNECTOR_POLL_SECONDS=0.01 \
  STEAMSHINE_GUARD_TEST_MARKER="${guard_marker}" \
  XDG_RUNTIME_DIR="${guard_runtime}" \
  "${root_dir}/scripts/steamshine-gamescope-session-guard.sh" "${test_root}/vendor-gamescope-session" &
guard_pid=$!
sleep 0.05
test ! -e "${guard_marker}"
rm -f "${guard_lease}"
wait "${guard_pid}"
grep -Fxq 'called' "${guard_marker}"

rm -f "${guard_marker}"
printf 'version=1\nboot_id=%s\nowner_pid=99999999\nowner_start_time=1\ngeneration=2\n' \
  "${guard_boot_id}" >"${guard_lease}"
chmod 600 "${guard_lease}"
STEAMSHINE_DRM_ROOT="${guard_drm}" \
  STEAMSHINE_CONNECTOR_POLL_SECONDS=0.01 \
  STEAMSHINE_GUARD_TEST_MARKER="${guard_marker}" \
  XDG_RUNTIME_DIR="${guard_runtime}" \
  "${root_dir}/scripts/steamshine-gamescope-session-guard.sh" "${test_root}/vendor-gamescope-session"
test ! -e "${guard_lease}"
grep -Fxq 'called' "${guard_marker}"

# PID reuse, a different boot, malformed data, and insecure mode are all stale
# rather than authoritative. The guard removes each regular owner file and
# immediately permits the connected vendor launcher.
for guard_invalid_case in pid-reuse boot-id malformed mode; do
  rm -f "${guard_marker}" "${guard_lease}"
  case "${guard_invalid_case}" in
    pid-reuse)
      printf 'version=1\nboot_id=%s\nowner_pid=%s\nowner_start_time=%s\ngeneration=3\n' \
        "${guard_boot_id}" "$$" "$((guard_owner_start + 1))" >"${guard_lease}"
      ;;
    boot-id)
      printf 'version=1\nboot_id=other-boot\nowner_pid=%s\nowner_start_time=%s\ngeneration=4\n' \
        "$$" "${guard_owner_start}" >"${guard_lease}"
      ;;
    malformed)
      printf 'not-a-valid-lease\n' >"${guard_lease}"
      ;;
    mode)
      printf 'version=1\nboot_id=%s\nowner_pid=%s\nowner_start_time=%s\ngeneration=5\n' \
        "${guard_boot_id}" "$$" "${guard_owner_start}" >"${guard_lease}"
      chmod 644 "${guard_lease}"
      ;;
  esac
  if [[ "${guard_invalid_case}" != mode ]]; then
    chmod 600 "${guard_lease}"
  fi
  STEAMSHINE_DRM_ROOT="${guard_drm}" \
    STEAMSHINE_CONNECTOR_POLL_SECONDS=0.01 \
    STEAMSHINE_GUARD_TEST_MARKER="${guard_marker}" \
    XDG_RUNTIME_DIR="${guard_runtime}" \
    "${root_dir}/scripts/steamshine-gamescope-session-guard.sh" "${test_root}/vendor-gamescope-session"
  test ! -e "${guard_lease}"
  grep -Fxq 'called' "${guard_marker}"
done

# A symlink can neither become a lease nor cause deletion of its target.
rm -f "${guard_marker}" "${guard_lease}"
guard_symlink_target="${test_root}/guard-symlink-target"
printf 'do-not-delete\n' >"${guard_symlink_target}"
ln -s "${guard_symlink_target}" "${guard_lease}"
STEAMSHINE_DRM_ROOT="${guard_drm}" \
  STEAMSHINE_CONNECTOR_POLL_SECONDS=0.01 \
  STEAMSHINE_GUARD_TEST_MARKER="${guard_marker}" \
  XDG_RUNTIME_DIR="${guard_runtime}" \
  "${root_dir}/scripts/steamshine-gamescope-session-guard.sh" "${test_root}/vendor-gamescope-session"
test -L "${guard_lease}"
grep -Fxq 'do-not-delete' "${guard_symlink_target}"
grep -Fxq 'called' "${guard_marker}"

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
grep -Fq 'Environment=XDG_RUNTIME_DIR=%t' "${root_dir}/packaging/linux/steamshine.service.in"
grep -Fq 'ExecStart=%h/.local/bin/steamshine %h/.config/steamshine/sunshine.conf' "${root_dir}/packaging/linux/steamshine.service.in"
grep -Fq 'WantedBy=default.target' "${root_dir}/packaging/linux/steamshine.service.in"
if grep -Eq 'graphical-session.target|User=deck|WAYLAND_DISPLAY=wayland-0|DISPLAY=:0' "${root_dir}/packaging/linux/steamshine.service.in"; then
  echo 'The packaged SteamShine user unit template is not Game Mode safe.' >&2
  exit 1
fi
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
mkdir -p "${test_root}/stage/bin" "${test_root}/stage/scripts" "${test_root}/home/run" "${test_root}/home/.config/sunshine"
install -m 755 /bin/true "${test_root}/stage/bin/steamshine"
install -m 755 /bin/true "${test_root}/stage/bin/steamshine-input-visualizer"
install -m 755 "${root_dir}/scripts/migrate-steamos-apps.py" "${test_root}/stage/scripts/migrate-steamos-apps.py"
install -m 755 "${root_dir}/scripts/steamshine-gamescope-session-guard.sh" "${test_root}/stage/scripts/steamshine-gamescope-session-guard.sh"
cat >"${test_root}/home/.config/sunshine/apps.json" <<'EOF'
{"env":{"CUSTOM":"preserved"},"apps":[{"name":"Desktop","image-path":"custom.png"},{"name":"Steam Big Picture","detached":["setsid env DISPLAY=:1 steam steam://open/bigpicture"]},{"name":"Custom Game","cmd":"custom-game"}]}
EOF
printf '{"target_architecture":"x86_64"}\n' >"${test_root}/stage/BUILD_INFO.json"
printf '{}\n' >"${test_root}/stage/STEAMOS_BASELINE.json"
tar --zstd -C "${test_root}/stage" -cf "${test_root}/steamshine-steamos-x86_64-test.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-x86_64-test.tar.zst >steamshine-steamos-x86_64-test.tar.zst.sha256)

# Plain `install` resolves the newest published GitHub Release, downloads its
# archive and detached checksum, and then enters the normal validated install.
release_commit='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
release_archive="steamshine-steamos-x86_64-${release_commit}.tar.zst"
mkdir -p "${test_root}/release-assets" "${test_root}/release-bin" "${test_root}/release-home/run"
cp "${test_root}/steamshine-steamos-x86_64-test.tar.zst" "${test_root}/release-assets/${release_archive}"
(cd "${test_root}/release-assets" && sha256sum "${release_archive}" >"${release_archive}.sha256")
cat >"${test_root}/release-assets/latest.json" <<EOF
{"tag_name":"steamos-test","assets":[{"name":"${release_archive}","browser_download_url":"https://github.com/souten-yd/SteamShine/releases/download/steamos-test/${release_archive}"},{"name":"${release_archive}.sha256","browser_download_url":"https://github.com/souten-yd/SteamShine/releases/download/steamos-test/${release_archive}.sha256"}]}
EOF
cat >"${test_root}/release-bin/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
output='' url=''
while (($#)); do
  case "$1" in
    --output) output="$2"; shift 2;;
    --proto|--tlsv1.2) [[ "$1" == --proto ]] && shift 2 || shift;;
    --fail|--location|--silent|--show-error) shift;;
    *) url="$1"; shift;;
  esac
done
case "${url}" in
  */releases/latest) cp "${RELEASE_METADATA}" "${output}";;
  *.tar.zst.sha256) cp "${RELEASE_CHECKSUM}" "${output}";;
  *.tar.zst) cp "${RELEASE_ARTIFACT}" "${output}";;
  *) exit 22;;
esac
EOF
chmod 755 "${test_root}/release-bin/curl"
RELEASE_METADATA="${test_root}/release-assets/latest.json" \
  RELEASE_ARTIFACT="${test_root}/release-assets/${release_archive}" \
  RELEASE_CHECKSUM="${test_root}/release-assets/${release_archive}.sha256" \
  STEAMSHINE_RELEASE_API_URL='https://api.github.test/repos/souten-yd/SteamShine/releases/latest' \
  HOME="${test_root}/release-home" XDG_RUNTIME_DIR="${test_root}/release-home/run" PATH="${test_root}/release-bin:${PATH}" \
  "${root_dir}/steamshine.sh" install --no-service --non-interactive --yes
test -x "${test_root}/release-home/.local/bin/steamshine"
test -f "${test_root}/release-home/.cache/steamshine/releases/${release_archive}"
grep -Fxq 'steamos_virtual_display_enabled = true' "${test_root}/release-home/.config/steamshine/sunshine.conf"

HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-service --non-interactive --yes
python3 - "${test_root}/home/.config/sunshine/apps.json" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
assert payload["env"]["CUSTOM"] == "preserved"
applications = {application["name"]: application for application in payload["apps"]}
assert "configure-steamos-client-display.py apply" in applications["Desktop"]["prep-cmd"][0]["do"]
assert applications["Steam Big Picture"]["detached"] == ["setsid steam steam://open/bigpicture"]
assert applications["Custom Game"]["cmd"] == "custom-game"
PY
test -f "${test_root}/home/.config/sunshine/apps.json.steamshine-backup"

# A relative file_apps setting follows Sunshine's app-data resolution instead
# of being interpreted relative to the installer working directory.
mkdir -p "${test_root}/custom-home/run" "${test_root}/custom-home/.config/steamshine" "${test_root}/custom-home/.config/sunshine/custom"
printf 'file_apps = custom/apps.json # preserve this location\n' >"${test_root}/custom-home/.config/steamshine/sunshine.conf"
printf '{"apps":[{"name":"Steam Big Picture"}]}\n' >"${test_root}/custom-home/.config/sunshine/custom/apps.json"
HOME="${test_root}/custom-home" XDG_RUNTIME_DIR="${test_root}/custom-home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-service --non-interactive --yes
python3 - "${test_root}/custom-home/.config/sunshine/custom/apps.json" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
assert "configure-steamos-client-display.py apply" in payload["apps"][0]["prep-cmd"][0]["do"]
PY
test -f "${test_root}/custom-home/.config/sunshine/custom/apps.json.steamshine-backup"

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
test -x "${test_root}/pr-home/.local/bin/steamshine-input-visualizer"
test -f "${test_root}/pr-home/.cache/steamshine/artifacts/800/steamshine-steamos-x86_64-deadbeef/steamshine-steamos-x86_64-deadbeef.tar.zst"

# The service passes Sunshine's configuration file as its positional argument.
# `--config` is not a Sunshine CLI option and would otherwise cause a restart
# loop before the host begins accepting Moonlight connections.
mkdir -p "${test_root}/mock-bin"
cat >"${test_root}/mock-bin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
state="${SYSTEMCTL_STATE_DIR:?}"
mkdir -p "${state}"
[[ -n "${SYSTEMCTL_LOG:-}" ]] && printf '%q ' "$@" >>"${SYSTEMCTL_LOG}"
[[ -n "${SYSTEMCTL_LOG:-}" ]] && printf '\n' >>"${SYSTEMCTL_LOG}"
[[ "${1:-}" == --user ]] && shift
command_name="${1:-}"
shift || true
unit_file="${HOME}/.config/systemd/user/steamshine.service"
wants="${HOME}/.config/systemd/user/default.target.wants/steamshine.service"
case "${command_name}" in
  show-environment|daemon-reload|reset-failed)
    exit 0
    ;;
  enable)
    touch "${state}/enabled"
    mkdir -p "$(dirname -- "${wants}")"
    ln -sfn ../steamshine.service "${wants}"
    ;;
  disable)
    rm -f "${state}/enabled" "${wants}"
    [[ " $* " == *' --now '* ]] && rm -f "${state}/active"
    ;;
  start|restart)
    [[ "${SYSTEMCTL_FAIL_START:-0}" == 1 ]] && exit 1
    touch "${state}/active"
    mkdir -p "${STEAMSHINE_PROC_ROOT:?}/4242"
    expected_binary="$(readlink -f -- "${HOME}/.local/bin/steamshine")"
    if [[ -n "${SYSTEMCTL_TRANSIENT_EXECUTABLE:-}" ]]; then
      ln -sfn "${SYSTEMCTL_TRANSIENT_EXECUTABLE}" "${STEAMSHINE_PROC_ROOT}/4242/exe"
      nohup bash -c 'sleep 0.1; ln -sfn "$1" "$2"' bash \
        "${expected_binary}" "${STEAMSHINE_PROC_ROOT}/4242/exe" >/dev/null 2>&1 &
    else
      ln -sfn "${expected_binary}" "${STEAMSHINE_PROC_ROOT}/4242/exe"
    fi
    ;;
  stop)
    rm -f "${state}/active"
    ;;
  is-enabled)
    [[ -f "${state}/enabled" ]] || exit 1
    [[ " $* " == *' --quiet '* ]] || printf 'enabled\n'
    ;;
  is-active)
    [[ -f "${state}/active" ]] || exit 1
    [[ " $* " == *' --quiet '* ]] || printf 'active\n'
    ;;
  show)
    property=''
    for argument in "$@"; do
      case "${argument}" in
        --property=*) property="${argument#--property=}";;
      esac
    done
    case "${property}" in
      LoadState) [[ -f "${unit_file}" ]] && printf 'loaded\n' || printf 'not-found\n';;
      ActiveState) [[ -f "${state}/active" ]] && printf 'active\n' || printf 'inactive\n';;
      SubState) [[ -f "${state}/active" ]] && printf 'running\n' || printf 'dead\n';;
      MainPID) [[ -f "${state}/active" ]] && printf '4242\n' || printf '0\n';;
      ExecStart) printf '{ path=%s/.local/bin/steamshine ; argv[]=%s/.local/bin/steamshine %s/.config/steamshine/sunshine.conf ; }\n' "${HOME}" "${HOME}" "${HOME}";;
      Environment) printf 'XDG_RUNTIME_DIR=/run/user/1000 STEAMSHINE_LAUNCH_MODE=systemd_user_service\n';;
    esac
    ;;
esac
EOF
chmod 755 "${test_root}/mock-bin/systemctl"
cat >"${test_root}/mock-bin/loginctl" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *--property=State*) printf 'active\n';;
  *--property=Linger*) printf 'no\n';;
esac
EOF
cat >"${test_root}/mock-bin/journalctl" <<'EOF'
#!/usr/bin/env bash
printf 'Started SteamShine game streaming host\n'
EOF
cat >"${test_root}/mock-bin/pgrep" <<'EOF'
#!/usr/bin/env bash
[[ -n "${MANUAL_STEAMSHINE_PID:-}" ]] && printf '%s\n' "${MANUAL_STEAMSHINE_PID}"
exit 0
EOF
chmod 755 "${test_root}/mock-bin/loginctl" "${test_root}/mock-bin/journalctl" "${test_root}/mock-bin/pgrep"
systemctl_state="${test_root}/systemctl-state"
mock_proc="${test_root}/mock-proc"
vendor_unit="${test_root}/gamescope-session.service"
mkdir -p "${systemctl_state}" "${mock_proc}"
printf '[Service]\n' >"${vendor_unit}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" \
  STEAMSHINE_GAMESCOPE_VENDOR_LAUNCHER="${test_root}/vendor-gamescope-session" \
  STEAMSHINE_GAMESCOPE_VENDOR_UNIT="${vendor_unit}" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --non-interactive --yes
service_unit="${test_root}/home/.config/systemd/user/steamshine.service"
guard_executable="${test_root}/home/.local/libexec/steamshine/steamshine-gamescope-session-guard"
guard_dropin="${test_root}/home/.config/systemd/user/gamescope-session.service.d/90-steamshine-headless-guard.conf"
grep -Fq 'ExecStart=%h/.local/bin/steamshine %h/.config/steamshine/sunshine.conf' "${service_unit}"
grep -Fq 'Environment=XDG_RUNTIME_DIR=%t' "${service_unit}"
grep -Fq 'Environment=PIPEWIRE_RUNTIME_DIR=%t' "${service_unit}"
grep -Fq 'Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=%t/bus' "${service_unit}"
grep -Fq 'Environment=STEAMSHINE_LAUNCH_MODE=systemd_user_service' "${service_unit}"
grep -Fq 'WantedBy=default.target' "${service_unit}"
if grep -Eq 'User=deck|PartOf=graphical-session.target|WAYLAND_DISPLAY=wayland-0|DISPLAY=:0' "${service_unit}"; then
  echo 'The user unit contains a forbidden Game Mode dependency or hard-coded session value.' >&2
  exit 1
fi
test -L "${test_root}/home/.config/systemd/user/default.target.wants/steamshine.service"
test -f "${systemctl_state}/enabled"
test -f "${systemctl_state}/active"
if grep -Fq -- '--config' "${service_unit}"; then
  echo 'The generated service must not pass an unsupported --config option.' >&2
  exit 1
fi
test -x "${test_root}/home/.local/bin/steamshine"
test -x "${test_root}/home/.local/bin/steamshine-input-visualizer"
test -x "${guard_executable}"
grep -Fq "ExecStart=%h/.local/libexec/steamshine/steamshine-gamescope-session-guard ${test_root}/vendor-gamescope-session" "${guard_dropin}"
grep -Fq 'TimeoutStartSec=infinity' "${guard_dropin}"
grep -Fq 'steamos_virtual_desktop_command = plasmawindowed org.kde.plasma.folder' "${test_root}/home/.config/steamshine/sunshine.conf"

# `--no-start` still establishes default.target autostart, while
# `--no-service` must leave the user manager and unit directory untouched.
no_start_home="${test_root}/no-start-home"
no_start_state="${test_root}/no-start-state"
no_start_proc="${test_root}/no-start-proc"
no_start_log="${test_root}/no-start-systemctl.log"
mkdir -p "${no_start_home}/run" "${no_start_state}" "${no_start_proc}"
HOME="${no_start_home}" XDG_RUNTIME_DIR="${no_start_home}/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${no_start_state}" STEAMSHINE_PROC_ROOT="${no_start_proc}" SYSTEMCTL_LOG="${no_start_log}" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-start --non-interactive --yes
test -f "${no_start_state}/enabled"
test ! -f "${no_start_state}/active"
test -L "${no_start_home}/.config/systemd/user/default.target.wants/steamshine.service"
if grep -Eq -- '--user (start|restart) ' "${no_start_log}"; then
  echo '--no-start must not start or restart SteamShine.' >&2
  exit 1
fi
test ! -e "${test_root}/release-home/.config/systemd/user/steamshine.service"

# Repeated installation is safe, and an active service is restarted so it
# cannot continue running an older immutable Artifact after the version switch.
service_unit_checksum="$(sha256sum "${service_unit}")"
systemctl_log="${test_root}/systemctl.log"
: >"${systemctl_log}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" SYSTEMCTL_LOG="${systemctl_log}" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --non-interactive --yes
test "$(sha256sum "${service_unit}")" = "${service_unit_checksum}"
grep -Fq -- '--user restart steamshine.service' "${systemctl_log}"

# Type=simple may briefly expose systemd's executor at MainPID before execve
# reaches the new immutable binary. Installation must retry that identity
# check for a bounded interval instead of reporting a false wrong_binary.
transient_executable="${test_root}/transient-systemd-executor"
printf '#!/bin/sh\nexit 0\n' >"${transient_executable}"
chmod 755 "${transient_executable}"
: >"${systemctl_log}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" SYSTEMCTL_LOG="${systemctl_log}" \
  SYSTEMCTL_TRANSIENT_EXECUTABLE="${transient_executable}" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --non-interactive --yes
test "$(readlink -f -- "${mock_proc}/4242/exe")" = "$(readlink -f -- "${test_root}/home/.local/bin/steamshine")"

# Repair recreates a missing default.target link without needlessly restarting
# an already-active process that resolves to the current installed binary.
rm "${test_root}/home/.config/systemd/user/default.target.wants/steamshine.service"
: >"${systemctl_log}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" SYSTEMCTL_LOG="${systemctl_log}" \
  "${root_dir}/steamshine.sh" repair --non-interactive --yes
test -L "${test_root}/home/.config/systemd/user/default.target.wants/steamshine.service"
if grep -Eq -- '--user (start|restart) ' "${systemctl_log}"; then
  echo 'Repair restarted a healthy active service.' >&2
  exit 1
fi

# An update of an enabled but inactive service preserves inactivity. A fake git
# confines this lifecycle check to update's service-state behavior.
cat >"${test_root}/mock-bin/git" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod 755 "${test_root}/mock-bin/git"
: >"${no_start_log}"
HOME="${no_start_home}" XDG_RUNTIME_DIR="${no_start_home}/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${no_start_state}" STEAMSHINE_PROC_ROOT="${no_start_proc}" SYSTEMCTL_LOG="${no_start_log}" \
  "${root_dir}/steamshine.sh" update --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --non-interactive --yes
test -f "${no_start_state}/enabled"
test ! -f "${no_start_state}/active"
if grep -Eq -- '--user (start|restart) ' "${no_start_log}"; then
  echo 'Update started an enabled service that was inactive before the update.' >&2
  exit 1
fi

# Diagnostics report the installed identity and flag a separately launched
# SteamShine process without terminating it.
autostart_output="$(HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" \
  "${root_dir}/steamshine.sh" autostart-status)"
grep -Fq 'autostart_health=healthy' <<<"${autostart_output}"
grep -Fq 'MainPID=4242' <<<"${autostart_output}"
if manual_output="$(HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" MANUAL_STEAMSHINE_PID=999 \
  "${root_dir}/steamshine.sh" autostart-status 2>&1)"; then
  echo 'A manual SteamShine process conflict was not reported as unhealthy.' >&2
  exit 1
fi
grep -Fq 'manual_process_conflict' <<<"${manual_output}"

# A failed start must propagate a nonzero installer result and must not print a
# misleading installation-success message.
failed_home="${test_root}/failed-home"
failed_state="${test_root}/failed-state"
failed_proc="${test_root}/failed-proc"
mkdir -p "${failed_home}/run" "${failed_state}" "${failed_proc}"
if failed_output="$(HOME="${failed_home}" XDG_RUNTIME_DIR="${failed_home}/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${failed_state}" STEAMSHINE_PROC_ROOT="${failed_proc}" SYSTEMCTL_FAIL_START=1 \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --non-interactive --yes 2>&1)"; then
  echo 'Expected a failed systemd start to fail installation.' >&2
  exit 1
fi
if grep -Fq 'installed and will start automatically' <<<"${failed_output}"; then
  echo 'Failed installation printed a success message.' >&2
  exit 1
fi

# A graphical launcher imports only non-empty desktop values before service
# activation, preserving an existing manager environment when a value is absent.
sed -i 's/^steamos_virtual_display_enabled = true$/steamos_virtual_display_enabled = false/' "${test_root}/home/.config/steamshine/sunshine.conf"
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u PIPEWIRE_REMOTE HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" WAYLAND_DISPLAY="wayland-0" PATH="${test_root}/mock-bin:${PATH}" SYSTEMCTL_LOG="${systemctl_log}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" \
  "${root_dir}/steamshine.sh" start --non-interactive --yes
grep -Fq -- '--user import-environment XDG_RUNTIME_DIR WAYLAND_DISPLAY' "${systemctl_log}"
if grep -Eq '(^|[[:space:]])DISPLAY([[:space:]]|$)' "${systemctl_log}"; then
  echo 'The launcher must not import an empty desktop variable.' >&2
  exit 1
fi
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
install -m 755 /bin/true "${test_root}/linked-stage/bin/steamshine-input-visualizer"
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
install -m 755 /bin/true "${test_root}/wrong-arch-stage/bin/steamshine-input-visualizer"
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
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/mock-bin:${PATH}" \
  SYSTEMCTL_STATE_DIR="${systemctl_state}" STEAMSHINE_PROC_ROOT="${mock_proc}" \
  "${root_dir}/steamshine.sh" uninstall --build-dir "${test_root}/cmake-build-steamos" --non-interactive --yes
test ! -e "${test_root}/home/.local/bin/steamshine"
test ! -e "${test_root}/home/.local/bin/steamshine-input-visualizer"
test ! -e "${test_root}/home/.local/share/steamshine/current"
test ! -d "${test_root}/home/.cache/steamshine"
test ! -e "${test_root}/home/.config/systemd/user/steamshine.service"
test ! -e "${test_root}/home/.config/systemd/user/default.target.wants/steamshine.service"
test ! -e "${guard_dropin}"
test ! -e "${guard_executable}"
test ! -f "${systemctl_state}/enabled"
test ! -f "${systemctl_state}/active"
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
# An explicitly stopped owned session leaves all three event classes in the
# user journal. Repeating this bounded fixture models the final cleanup.
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
  printf '101\n' >"${session}/gamescope.pid"
  if [[ ! -S "${session}/gamescope-0" ]]; then
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
  fi
  for _ in $(seq 1 20); do [[ -S "${session}/gamescope-0" ]] && break; sleep 0.01; done
  printf 'XDG_RUNTIME_DIR=%s\0' "${session}" >"${proc_root}/101/environ"
elif [[ "${event}" == stopped ]]; then
  if [[ -r "${session}/socket.pid" ]]; then kill "$(<"${session}/socket.pid")" 2>/dev/null || true; fi
  rm -rf -- "${session}" "${proc_root}/101"
fi
EOF
chmod 755 "${test_root}/hardware-bin/event-hook"
for _ in $(seq 1 21); do printf '\n'; done >"${acceptance_input}"
for _ in video audio keyboard mouse gamepad; do printf 'y\n'; done >>"${acceptance_input}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/hardware-bin:${PATH}" \
  STEAMSHINE_BINARY="${test_root}/hardware-bin/steamshine" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_acceptance_report}" \
  STEAMSHINE_HARDWARE_SAMPLE_SECONDS=0 STEAMSHINE_TEST_MODE=1 STEAMSHINE_TEST_EVENT_HOOK="${test_root}/hardware-bin/event-hook" \
  "${root_dir}/scripts/test-steamos-virtual-display.sh" <"${acceptance_input}"
grep -Fq '"result": "pass"' "${hardware_acceptance_report}/hardware-report.json"
grep -Fq '"connect_disconnect_cycles": 10' "${hardware_acceptance_report}/hardware-report.json"
test "$(wc -l <"${hardware_acceptance_report}/encoded-stream-evidence.tsv")" -eq 1
grep -Fq '"captured_frame_count": 60' "${hardware_acceptance_report}/hardware-report.json"
test "$(grep -c '^owned_session_evidence=connected-' "${hardware_acceptance_report}/virtual-display.log")" -eq 10
test "$(grep -c '^owned_session_evidence=disconnected-' "${hardware_acceptance_report}/virtual-display.log")" -eq 10
grep -Fq '"disconnect_retains_owned_session_evidence": true' "${hardware_acceptance_report}/hardware-report.json"
