#!/usr/bin/env bash
# SteamShine SteamOS lifecycle entry point. Run from the repository root.
# shellcheck disable=SC1091,SC2015,SC2034,SC2155
set -Eeuo pipefail

readonly EXIT_USAGE=2 EXIT_UNSUPPORTED=3 EXIT_DEPENDENCY=4 EXIT_BUILD=6 EXIT_TEST=7 EXIT_SERVICE=8 EXIT_CONFIG=9 EXIT_UNINSTALL=10
readonly SERVICE_UNIT=steamshine.service
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PREFIX="${HOME}/.local"
BUILD_DIR="${ROOT_DIR}/cmake-build-steamos"
CONFIG_FILE="${HOME}/.config/steamshine/sunshine.conf"
STATE_DIR="${HOME}/.local/state/steamshine"
DRY_RUN=false NON_INTERACTIVE=false ASSUME_YES=false VERBOSE=false QUIET=false FORCE=false NO_START=false NO_BUILD=false NO_PACKAGES=false NO_SERVICE=false PURGE=false REMOVE_DEPENDENCIES=false CLEAN=false HARDWARE_INTERACTIVE=false AUTO_RELEASE=false BUILD_TYPE=Release
GAME_GPU="" CAPTURE_GPU="" ENCODER_GPU="" GAMESCOPE_PATH="gamescope" DEFAULT_WIDTH=1920 DEFAULT_HEIGHT=1080 DEFAULT_FPS=60
CHANNEL="stable" PR_NUMBER="" RELEASE_TAG="" ARTIFACT_PATH=""

say() { "${QUIET}" || printf '%s\n' "$*"; }
die() { printf 'steamshine: %s\n' "$*" >&2; exit "${2:-1}"; }
run() { if "${DRY_RUN}"; then printf '[dry-run]'; printf ' %q' "$@"; printf '\n'; else "$@"; fi; }
json_value() { local key="$1" file="$2"; sed -nE "s/.*\"${key}\"[[:space:]]*:[[:space:]]*\"([^\"]*)\".*/\1/p" "${file}" | head -n1; }
version_at_least() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }
path_within() { local candidate="$1" parent="$2" canonical_candidate canonical_parent; canonical_candidate="$(realpath -m -- "${candidate}")"; canonical_parent="$(realpath -m -- "${parent}")"; [[ "${canonical_candidate}" == "${canonical_parent}" || "${canonical_candidate}" == "${canonical_parent}/"* ]]; }
usage() { cat <<'EOF'
Usage: ./steamshine.sh <command> [options]
Commands: menu check compatibility-check vaapi-driver-status install build configure start stop restart status logs diagnose autostart-status update repair uninstall bootstrap rollback hardware-test
Options: -a, --latest-release --non-interactive --yes --dry-run --verbose --quiet --force --no-start --no-build --no-packages --no-service --config PATH --prefix PATH --build-dir PATH --channel stable|nightly|pr --pr NUMBER --release TAG --artifact PATH --game-gpu ID --capture-gpu ID --encoder-gpu ID --gamescope-path PATH --default-width PX --default-height PX --default-fps FPS --log-file PATH --purge --remove-dependencies --clean --debug --release
EOF
}
require_bash() { [[ -n "${BASH_VERSION:-}" ]] || die 'Run this script with bash.' "$EXIT_USAGE"; }
parse() {
  COMMAND="${1:-}"; [[ $# -gt 0 ]] && shift || true
  if [[ "${COMMAND}" == "-h" || "${COMMAND}" == "--help" ]]; then usage; exit 0; fi
  while [[ $# -gt 0 ]]; do case "$1" in
    -a|--latest-release) AUTO_RELEASE=true;; --non-interactive) NON_INTERACTIVE=true;; --interactive) HARDWARE_INTERACTIVE=true;; --yes) ASSUME_YES=true;; --dry-run) DRY_RUN=true;; --verbose) VERBOSE=true;; --quiet) QUIET=true;; --force) FORCE=true;; --no-start) NO_START=true;; --no-build) NO_BUILD=true;; --no-packages) NO_PACKAGES=true;; --no-service) NO_SERVICE=true;; --purge) PURGE=true;; --remove-dependencies) REMOVE_DEPENDENCIES=true;; --clean) CLEAN=true;; --debug) BUILD_TYPE=Debug;; --release) BUILD_TYPE=Release;;
    --config|--prefix|--build-dir|--log-file|--channel|--pr|--artifact|--game-gpu|--capture-gpu|--encoder-gpu|--gamescope-path|--default-width|--default-height|--default-fps) [[ $# -ge 2 ]] || die "Missing value for $1" "$EXIT_USAGE"; case "$1" in --config) CONFIG_FILE="$2";; --prefix) PREFIX="$2";; --build-dir) BUILD_DIR="$2";; --channel) CHANNEL="$2";; --pr) PR_NUMBER="$2";; --artifact) ARTIFACT_PATH="$2";; --game-gpu) GAME_GPU="$2";; --capture-gpu) CAPTURE_GPU="$2";; --encoder-gpu) ENCODER_GPU="$2";; --gamescope-path) GAMESCOPE_PATH="$2";; --default-width) DEFAULT_WIDTH="$2";; --default-height) DEFAULT_HEIGHT="$2";; --default-fps) DEFAULT_FPS="$2";; esac; shift;;
    -h|--help) usage; exit 0;; *) die "Unknown option: $1" "$EXIT_USAGE";; esac; shift; done
  if "${AUTO_RELEASE}" && { [[ -n "${ARTIFACT_PATH}" ]] || [[ -n "${PR_NUMBER}" ]]; }; then
    die '-a/--latest-release cannot be combined with --artifact or --pr.' "$EXIT_USAGE"
  fi
}
load_os_release() { [[ -r /etc/os-release ]] || die '/etc/os-release is required.' "$EXIT_UNSUPPORTED"; . /etc/os-release; }
package_manager() { load_os_release; case "${ID}:${ID_LIKE:-}" in steamos:*|arch:*) printf 'pacman\n';; ubuntu:*|debian:*|*:*debian*) printf 'apt\n';; fedora:*|*:*fedora*) printf 'dnf\n';; *) die "Unsupported Linux distribution: ${ID}" "$EXIT_UNSUPPORTED";; esac; }
is_steamos_or_arch() { load_os_release; [[ "${ID}" == steamos || "${ID}" == arch ]]; }
check() {
  say '[1/5] Checking supported environment'; package_manager >/dev/null
  say '[2/5] Checking user runtime directory'; [[ -n "${XDG_RUNTIME_DIR:-}" && -d "${XDG_RUNTIME_DIR}" ]] || die 'XDG_RUNTIME_DIR is required.' "$EXIT_DEPENDENCY"
  say '[3/5] Checking GPU access'; [[ -r /dev/dri/renderD128 || -r /dev/dri/card0 ]] || die 'No accessible DRM device.' "$EXIT_DEPENDENCY"
  say '[4/5] Checking runtime commands'; command -v systemctl >/dev/null || die 'systemctl --user is required.' "$EXIT_DEPENDENCY"
  say '[5/5] Checking virtual-display prerequisites'; command -v gamescope >/dev/null || say 'Gamescope is required when steamos_virtual_display_enabled=true.'
  say 'Environment check passed'
}
build_check() {
  command -v cmake >/dev/null && command -v ninja >/dev/null && command -v pkg-config >/dev/null && command -v clang-format >/dev/null && command -v shellcheck >/dev/null || die 'cmake, ninja, pkg-config, clang-format, and shellcheck are required for local builds.' "$EXIT_DEPENDENCY"
  local cmake_version
  cmake_version="$(cmake --version | awk 'NR == 1 {print $3}')"
  [[ "$(printf '%s\n%s\n' 3.25.1 "${cmake_version}" | sort -V | head -n1)" == 3.25.1 ]] || die "cmake >= 3.25.1 is required (found ${cmake_version})." "$EXIT_DEPENDENCY"
}
install_packages() {
  local manager; manager="$(package_manager)"; command -v "${manager}" >/dev/null || die "${manager} is unavailable." "$EXIT_DEPENDENCY"
  if ! "${DRY_RUN}" && ! sudo -n true 2>/dev/null; then
    die 'Package installation needs sudo authorization. Run the command from an interactive terminal after authenticating with sudo, or configure an approved askpass helper.' "$EXIT_DEPENDENCY"
  fi
  local packages=() available=() package
  case "${manager}" in
    pacman) packages=(base-devel cmake ninja pkgconf git python python-jinja nodejs npm clang shellcheck libcap libdrm libevdev libnotify libpulse libva libx11 libxcb libxfixes libxrandr libxtst miniupnpc openssl opus qt6-base qt6-svg shaderc udev vulkan-icd-loader vulkan-tools wayland pipewire gamescope libva-utils);;
    apt) packages=(build-essential cmake ninja-build pkg-config git python3 python3-jinja2 npm clang-format shellcheck libcap-dev libdrm-dev libevdev-dev libgbm-dev libminiupnpc-dev libnotify-dev libnuma-dev libopus-dev libpipewire-0.3-dev libpulse-dev libssl-dev libsystemd-dev libudev-dev libwayland-dev libx11-dev libx11-xcb-dev libxcb-dri3-dev libxcb-shm0-dev libxcb-xfixes0-dev libxfixes-dev libxrandr-dev libxtst-dev libvulkan-dev vulkan-tools vainfo gamescope pipewire);;
    dnf) packages=(gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git python3 python3-jinja2 nodejs npm clang-tools-extra ShellCheck libcap-devel libdrm-devel libevdev-devel libnotify-devel libva-devel libX11-devel libxcb-devel libXfixes-devel libXrandr-devel libXtst-devel miniupnpc-devel openssl-devel opus-devel pipewire-devel pulseaudio-libs-devel systemd-devel libudev-devel wayland-devel vulkan-loader-devel vulkan-tools gamescope libva-utils);;
  esac
  for package in "${packages[@]}"; do
    case "${manager}" in pacman) pacman -Si "${package}" >/dev/null 2>&1 && available+=("${package}");; apt) apt-cache show "${package}" >/dev/null 2>&1 && available+=("${package}");; dnf) dnf -q info "${package}" >/dev/null 2>&1 && available+=("${package}");; esac
  done
  ((${#available[@]})) || die 'No verified dependency packages are available from the configured package manager.' "$EXIT_DEPENDENCY"
  if ! "${ASSUME_YES}" && ! "${NON_INTERACTIVE}"; then read -r -p "Install missing official packages with pacman? [y/N] " answer; [[ "${answer}" =~ ^[Yy]$ ]] || return 0; fi
  case "${manager}" in pacman) run sudo pacman -S --needed --noconfirm "${available[@]}";; apt) run sudo apt-get update; run sudo apt-get install -y "${available[@]}";; dnf) run sudo dnf install -y "${available[@]}";; esac
  if ! "${DRY_RUN}"; then mkdir -p "${STATE_DIR}"; printf '%s\n' "${available[@]}" >"${STATE_DIR}/installed-packages.txt"; fi
}
build() {
  build_check
  "${CLEAN}" && run cmake -E rm -rf "${BUILD_DIR}"
  run cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DBUILD_TESTS=ON
  run cmake --build "${BUILD_DIR}" --parallel "$(nproc)" || die 'Build failed.' "$EXIT_BUILD"
  run ctest --test-dir "${BUILD_DIR}" --output-on-failure || die 'CTest failed.' "$EXIT_TEST"
}
configure() {
  local directory="$(dirname -- "${CONFIG_FILE}")"; run mkdir -p "${directory}" "${directory}/backups"
  if [[ -e "${CONFIG_FILE}" ]]; then say 'Already configured; preserving existing configuration.'; return; fi
  if "${DRY_RUN}"; then say "[dry-run] create ${CONFIG_FILE}"; return; fi
  cat >"${CONFIG_FILE}" <<EOF
# SteamShine recommended SteamOS settings.
steamos_virtual_display_enabled = true
steamos_virtual_display_mode = auto
steamos_session_source = auto
steamos_gamescope_path = ${GAMESCOPE_PATH}
steamos_virtual_desktop_command = plasmawindowed org.kde.plasma.folder
steamos_runtime_directory = ${XDG_RUNTIME_DIR}/steamshine
steamos_game_gpu = ${GAME_GPU}
steamos_capture_gpu = ${CAPTURE_GPU}
steamos_encoder_gpu = ${ENCODER_GPU}
steamos_startup_timeout_seconds = 15
steamos_shutdown_timeout_seconds = 5
steamos_default_width = ${DEFAULT_WIDTH}
steamos_default_height = ${DEFAULT_HEIGHT}
steamos_default_fps = ${DEFAULT_FPS}
steamos_cleanup_orphan_sessions = true
EOF
}
# Apply the recommended Game Mode capture policy without replacing unrelated
# Sunshine settings. Duplicate keys are collapsed so the result is unambiguous.
configure_recommended() {
  [[ -e "${CONFIG_FILE}" ]] || configure
  if "${DRY_RUN}"; then
    say "[dry-run] set recommended SteamOS settings in ${CONFIG_FILE}"
    return
  fi
  local temporary backup_directory
  temporary="$(mktemp "${CONFIG_FILE}.XXXXXX")"
  backup_directory="$(dirname -- "${CONFIG_FILE}")/backups"
  if ! awk '
    BEGIN {
      recommended["steamos_virtual_display_enabled"] = "true"
      recommended["steamos_virtual_display_mode"] = "auto"
      recommended["steamos_session_source"] = "auto"
    }
    {
      matched = ""
      for (key in recommended) {
        if ($0 ~ "^[[:space:]]*" key "[[:space:]]*=") {
          matched = key
          break
        }
      }
      if (matched != "") {
        if (!written[matched]++) {
          print matched " = " recommended[matched]
        }
        next
      }
      print
    }
    END {
      for (key in recommended) {
        if (!written[key]) {
          print key " = " recommended[key]
        }
      }
    }
  ' "${CONFIG_FILE}" >"${temporary}"; then
    rm -f -- "${temporary}"
    die 'Recommended SteamOS settings could not be generated.' "${EXIT_CONFIG}"
  fi
  if cmp -s -- "${CONFIG_FILE}" "${temporary}"; then
    rm -f -- "${temporary}"
    say 'Recommended SteamOS settings are already configured'
    return
  fi
  mkdir -p "${backup_directory}"
  if [[ ! -f "${backup_directory}/sunshine.conf.before-recommended-settings" ]]; then
    cp -- "${CONFIG_FILE}" "${backup_directory}/sunshine.conf.before-recommended-settings"
  fi
  chmod --reference="${CONFIG_FILE}" "${temporary}"
  mv -f -- "${temporary}" "${CONFIG_FILE}"
  say 'Recommended SteamOS settings applied'
}
service_file() { printf '%s\n' "${HOME}/.config/systemd/user/${SERVICE_UNIT}"; }
service_wants_link() { printf '%s\n' "${HOME}/.config/systemd/user/default.target.wants/${SERVICE_UNIT}"; }
gamescope_guard_dropin() { printf '%s\n' "${HOME}/.config/systemd/user/gamescope-session.service.d/90-steamshine-headless-guard.conf"; }
gamescope_guard_executable() { printf '%s\n' "${PREFIX}/libexec/steamshine/steamshine-gamescope-session-guard"; }
systemd_user_path() {
  local path="$1"
  if [[ "${path}" == "${HOME}" ]]; then
    printf '%%h\n'
  elif [[ "${path}" == "${HOME}/"* ]]; then
    printf '%%h/%s\n' "${path#"${HOME}/"}"
  else
    printf '%s\n' "${path}"
  fi
}
install_gamescope_session_guard() {
  local source="${PREFIX}/share/steamshine/current/scripts/steamshine-gamescope-session-guard.sh"
  local vendor_launcher="${STEAMSHINE_GAMESCOPE_VENDOR_LAUNCHER:-/usr/lib/steamos/gamescope-session}"
  local vendor_unit="${STEAMSHINE_GAMESCOPE_VENDOR_UNIT:-/usr/lib/systemd/user/gamescope-session.service}"
  local executable dropin dropin_directory temporary systemd_executable
  executable="$(gamescope_guard_executable)"
  dropin="$(gamescope_guard_dropin)"
  if ! virtual_display_enabled; then
    run rm -f "${dropin}" "${executable}"
    run systemctl --user daemon-reload || true
    return
  fi
  [[ -x "${vendor_launcher}" && -f "${vendor_unit}" && -x "${source}" ]] || return 0
  dropin_directory="$(dirname -- "${dropin}")"
  systemd_executable="$(systemd_user_path "${executable}")"
  run mkdir -p "$(dirname -- "${executable}")" "${dropin_directory}"
  if "${DRY_RUN}"; then
    say "[dry-run] install ${executable} and ${dropin}"
    return
  fi
  command install -m 0755 "${source}" "${executable}"
  temporary="$(mktemp "${dropin}.XXXXXX")"
  if ! cat >"${temporary}" <<EOF
[Service]
ExecStart=
ExecStart=${systemd_executable} ${vendor_launcher}
TimeoutStartSec=infinity
EOF
  then
    rm -f -- "${temporary}"
    die 'The headless Game Mode session guard could not be generated.' "${EXIT_SERVICE}"
  fi
  chmod 0644 "${temporary}"
  mv -f -- "${temporary}" "${dropin}"
  systemctl --user daemon-reload || die 'The systemd user manager could not load the headless Game Mode session guard.' "${EXIT_SERVICE}"
}
install_service() {
  local unit unit_directory temporary executable config
  unit="$(service_file)"
  unit_directory="$(dirname -- "${unit}")"
  executable="$(systemd_user_path "${PREFIX}/bin/steamshine")"
  config="$(systemd_user_path "${CONFIG_FILE}")"
  run mkdir -p "${unit_directory}"
  if "${DRY_RUN}"; then say "[dry-run] create ${unit}"; return; fi
  temporary="$(mktemp "${unit}.XXXXXX")"
  if ! cat >"${temporary}" <<EOF
[Unit]
Description=SteamShine game streaming host
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
Environment=XDG_RUNTIME_DIR=%t
Environment=PIPEWIRE_RUNTIME_DIR=%t
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=%t/bus
Environment=STEAMSHINE_LAUNCH_MODE=systemd_user_service
ExecStart=${executable} ${config}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=default.target
EOF
  then
    rm -f -- "${temporary}"
    die 'The SteamShine user unit could not be generated.' "${EXIT_SERVICE}"
  fi
  chmod 0644 "${temporary}"
  mv -f -- "${temporary}" "${unit}"
  systemctl --user daemon-reload || die 'The systemd user manager could not reload the SteamShine unit.' "${EXIT_SERVICE}"
}
enable_service() {
  run systemctl --user enable "${SERVICE_UNIT}" || die 'The SteamShine user service could not be enabled.' "${EXIT_SERVICE}"
}
systemctl_value() {
  local property="$1"
  systemctl --user show "${SERVICE_UNIT}" --property="${property}" --value 2>/dev/null || true
}
service_enabled() { systemctl --user is-enabled --quiet "${SERVICE_UNIT}" 2>/dev/null; }
service_active() { systemctl --user is-active --quiet "${SERVICE_UNIT}" 2>/dev/null; }
service_main_pid() { systemctl_value MainPID; }
service_process_executable() {
  local pid="$1" proc_root="${STEAMSHINE_PROC_ROOT:-/proc}"
  [[ "${pid}" =~ ^[1-9][0-9]*$ ]] || return 1
  readlink -f -- "${proc_root}/${pid}/exe" 2>/dev/null
}
verify_service() {
  local require_active="$1" load_state main_pid process_executable expected_executable exec_start attempt
  "${DRY_RUN}" && return 0
  [[ -f "$(service_file)" ]] || die 'SteamShine autostart verification failed: unit_missing.' "${EXIT_SERVICE}"
  service_enabled || die 'SteamShine autostart verification failed: unit_not_enabled.' "${EXIT_SERVICE}"
  [[ -L "$(service_wants_link)" ]] || die 'SteamShine autostart verification failed: wants_symlink_missing.' "${EXIT_SERVICE}"
  [[ -x "${PREFIX}/bin/steamshine" ]] || die 'SteamShine autostart verification failed: executable_missing.' "${EXIT_SERVICE}"
  [[ -f "${CONFIG_FILE}" ]] || die 'SteamShine autostart verification failed: config_missing.' "${EXIT_SERVICE}"
  load_state="$(systemctl_value LoadState)"
  [[ "${load_state}" == loaded ]] || die "SteamShine autostart verification failed: unit load state is ${load_state:-unknown}." "${EXIT_SERVICE}"
  exec_start="$(systemctl_value ExecStart)"
  [[ "${exec_start}" == *"${PREFIX}/bin/steamshine"* && "${exec_start}" == *"${CONFIG_FILE}"* ]] || die 'SteamShine autostart verification failed: ExecStart does not reference the installed binary and configuration.' "${EXIT_SERVICE}"
  if "${require_active}"; then
    service_active || die 'SteamShine autostart verification failed: service_failed.' "${EXIT_SERVICE}"
    expected_executable="$(readlink -f -- "${PREFIX}/bin/steamshine")"
    # A Type=simple unit becomes active when systemd forks its executor, just
    # before that process finishes execve() into the new immutable binary.
    # Bound the identity check so a successful update is not rejected during
    # this short transition, while a genuinely stale process still fails.
    for ((attempt = 0; attempt < 50; ++attempt)); do
      main_pid="$(service_main_pid)"
      process_executable="$(service_process_executable "${main_pid}" || true)"
      if [[ "${main_pid}" =~ ^[1-9][0-9]*$ && "${process_executable}" == "${expected_executable}" ]]; then
        break
      fi
      sleep 0.05
    done
    [[ "${main_pid}" =~ ^[1-9][0-9]*$ ]] || die 'SteamShine autostart verification failed: MainPID is unavailable.' "${EXIT_SERVICE}"
    [[ "${process_executable}" == "${expected_executable}" ]] || die 'SteamShine autostart verification failed: wrong_binary.' "${EXIT_SERVICE}"
  fi
}
fetch_artifact() {
  [[ -n "${ARTIFACT_PATH}" ]] && return
  if "${AUTO_RELEASE}" || [[ "${CHANNEL}" == stable ]]; then
    fetch_latest_release
    return
  fi
  command -v gh >/dev/null || die 'Install gh or download the PR artifact from Actions and pass --artifact <path>.' "$EXIT_DEPENDENCY"
  local cache="${HOME}/.cache/steamshine/artifacts" run_id="" candidate artifact_name="" artifact_names run_cache
  run mkdir -p "${cache}"
  if [[ "${CHANNEL}" != pr ]]; then
    die 'Use the stable channel, --artifact for a local artifact, or --channel pr --pr NUMBER.' "$EXIT_USAGE"
  fi
  [[ -n "${PR_NUMBER}" ]] || die '--channel pr requires --pr NUMBER.' "$EXIT_USAGE"
  # A later docs-only run can succeed without producing a delivery archive.
  # Select the most recent successful run that actually owns the immutable
  # SteamOS Artifact, rather than trusting the latest workflow conclusion.
  while IFS= read -r candidate; do
    [[ "${candidate}" =~ ^[0-9]+$ ]] || continue
    if ! artifact_names="$(gh api "repos/souten-yd/SteamShine/actions/runs/${candidate}/artifacts" --jq '.artifacts[] | select(.expired == false) | .name' 2>/dev/null)"; then
      continue
    fi
    artifact_name="$(grep -E '^steamshine-steamos-x86_64-[[:xdigit:]]+$' <<<"${artifact_names}" | head -n1 || true)"
    if [[ -n "${artifact_name}" ]]; then
      run_id="${candidate}"
      break
    fi
  done < <(gh run list --repo souten-yd/SteamShine --branch "$(gh pr view "${PR_NUMBER}" --json headRefName -q .headRefName)" --workflow build-steamos.yml --limit 30 --json databaseId,conclusion --jq '.[] | select(.conclusion == "success") | .databaseId')
  [[ -n "${run_id}" ]] || die 'No successful build-steamos run with a SteamOS delivery artifact was found for this PR.' "$EXIT_DEPENDENCY"
  run_cache="${cache}/${run_id}"
  run mkdir -p "${run_cache}"
  run gh run download "${run_id}" --repo souten-yd/SteamShine --name "${artifact_name}" --dir "${run_cache}"
  ARTIFACT_PATH="$(find "${run_cache}" -type f -name 'steamshine-steamos-*.tar.zst' -print -quit)"
  [[ -n "${ARTIFACT_PATH}" ]] || die "Artifact download did not contain ${artifact_name}." "$EXIT_DEPENDENCY"
}
fetch_latest_release() {
  command -v curl >/dev/null || die 'curl is required to download the latest GitHub release.' "${EXIT_DEPENDENCY}"
  command -v python3 >/dev/null || die 'python3 is required to inspect the latest GitHub release.' "${EXIT_DEPENDENCY}"
  local api_url="${STEAMSHINE_RELEASE_API_URL:-https://api.github.com/repos/souten-yd/SteamShine/releases/latest}"
  local cache="${HOME}/.cache/steamshine/releases" metadata temporary_artifact temporary_checksum
  local -a release=()
  run mkdir -p "${cache}"
  metadata="$(mktemp "${cache}/latest-release.XXXXXX.json")"
  if ! curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error --output "${metadata}" "${api_url}"; then
    rm -f -- "${metadata}"
    die 'The latest GitHub release metadata could not be downloaded.' "${EXIT_DEPENDENCY}"
  fi
  mapfile -t release < <(python3 - "${metadata}" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as release_file:
    payload = json.load(release_file)

assets = {asset.get("name", ""): asset.get("browser_download_url", "") for asset in payload.get("assets", [])}
archives = [name for name in assets if re.fullmatch(r"steamshine-steamos-x86_64-[0-9A-Fa-f]+\.tar\.zst", name)]
if len(archives) != 1:
    raise SystemExit("latest release must contain exactly one SteamOS x86_64 archive")
archive = archives[0]
checksum = f"{archive}.sha256"
if checksum not in assets:
    raise SystemExit("latest release is missing the archive checksum")
expected_prefix = "https://github.com/souten-yd/SteamShine/releases/download/"
if not assets[archive].startswith(expected_prefix) or not assets[checksum].startswith(expected_prefix):
    raise SystemExit("latest release contains an unexpected download URL")
print(payload.get("tag_name", ""))
print(archive)
print(assets[archive])
print(checksum)
print(assets[checksum])
PY
  )
  rm -f -- "${metadata}"
  [[ ${#release[@]} -eq 5 && -n "${release[0]}" ]] || die 'The latest GitHub release assets are invalid.' "${EXIT_DEPENDENCY}"
  temporary_artifact="$(mktemp "${cache}/${release[1]}.XXXXXX")"
  temporary_checksum="$(mktemp "${cache}/${release[3]}.XXXXXX")"
  if ! curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error --output "${temporary_artifact}" "${release[2]}" ||
    ! curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error --output "${temporary_checksum}" "${release[4]}"; then
    rm -f -- "${temporary_artifact}" "${temporary_checksum}"
    die "SteamShine ${release[0]} could not be downloaded." "${EXIT_DEPENDENCY}"
  fi
  mv -f -- "${temporary_artifact}" "${cache}/${release[1]}"
  mv -f -- "${temporary_checksum}" "${cache}/${release[3]}"
  ARTIFACT_PATH="${cache}/${release[1]}"
  say "Downloaded SteamShine ${release[0]}"
}
validate_artifact() {
  local artifact="$1" entry
  [[ -f "${artifact}" ]] || die 'A local .tar.zst artifact is required.' "$EXIT_DEPENDENCY"
  # The delivery archive has no reason to contain links. Rejecting both link
  # types avoids a symlink or hard-link escape between validation and extraction.
  if tar --zstd -tvf "${artifact}" | grep -Eq '^[lh]'; then
    die 'Archive symlinks and hard links are rejected.' "$EXIT_DEPENDENCY"
  fi
  while IFS= read -r entry; do
    [[ -n "${entry}" ]] || continue
    [[ "${entry}" != /* && "${entry}" != ../* && "${entry}" != */../* && "${entry}" != .. ]] || die 'Unsafe archive path rejected.' "$EXIT_DEPENDENCY"
  done < <(tar --zstd -tf "${artifact}")
}
install_artifact() {
  if "${DRY_RUN}"; then say "[dry-run] verify and install artifact ${ARTIFACT_PATH:-for channel ${CHANNEL}} below ${PREFIX}"; return; fi
  fetch_artifact
  [[ "$(uname -m)" == x86_64 ]] || die 'This artifact supports x86_64 only.' "$EXIT_UNSUPPORTED"
  local checksum="${ARTIFACT_PATH}.sha256" target="${PREFIX}/share/steamshine" versions="${PREFIX}/share/steamshine/versions" extract version previous=""
  [[ -f "${checksum}" ]] || die "Missing checksum: ${checksum}" "$EXIT_DEPENDENCY"
  (cd -- "$(dirname -- "${checksum}")" && sha256sum -c "$(basename -- "${checksum}")") || die 'Artifact checksum mismatch.' "$EXIT_DEPENDENCY"
  validate_artifact "${ARTIFACT_PATH}"
  mkdir -p "${HOME}/.cache/steamshine"; extract="$(mktemp -d "${HOME}/.cache/steamshine/extract.XXXXXX")"
  if ! tar --zstd -C "${extract}" -xf "${ARTIFACT_PATH}"; then rm -rf -- "${extract}"; die 'Artifact extraction failed.' "$EXIT_DEPENDENCY"; fi
  if [[ ! -x "${extract}/bin/steamshine" || ! -x "${extract}/bin/steamshine-input-visualizer" || ! -f "${extract}/BUILD_INFO.json" || ! -f "${extract}/STEAMOS_BASELINE.json" ]]; then rm -rf -- "${extract}"; die 'Artifact layout is invalid.' "$EXIT_DEPENDENCY"; fi
  [[ "$(json_value target_architecture "${extract}/BUILD_INFO.json")" == x86_64 ]] || { rm -rf -- "${extract}"; die 'Artifact architecture is not x86_64.' "$EXIT_UNSUPPORTED"; }
  run mkdir -p "${versions}" "${PREFIX}/bin"; version="$(sha256sum "${ARTIFACT_PATH}" | awk '{print $1}')"
  if [[ -L "${target}/current" ]]; then previous="$(readlink -f -- "${target}/current" || true)"; fi
  if [[ -e "${versions}/${version}" ]]; then
    rm -rf -- "${extract}"
    say 'Already installed'
  else
    run mv "${extract}" "${versions}/${version}"
  fi
  if [[ -n "${previous}" && "${previous}" != "${versions}/${version}" ]] && path_within "${previous}" "${versions}"; then
    if "${DRY_RUN}"; then
      say "[dry-run] record rollback target ${previous}"
    else
      printf '%s\n' "${previous}" >"${target}/rollback"
    fi
  fi
  if ! "${DRY_RUN}"; then
    ln -s "${versions}/${version}" "${target}/current.next"
    mv -Tf "${target}/current.next" "${target}/current"
    ln -sfn "${target}/current/bin/steamshine" "${PREFIX}/bin/steamshine"
    ln -sfn "${target}/current/bin/steamshine-input-visualizer" "${PREFIX}/bin/steamshine-input-visualizer"
  fi
}
configured_apps_file() {
  local configured=""
  if [[ -r "${CONFIG_FILE}" ]]; then
    configured="$(sed -nE 's/^[[:space:]]*file_apps[[:space:]]*=[[:space:]]*([^#]*[^#[:space:]])[[:space:]]*(#.*)?$/\1/p' "${CONFIG_FILE}" | tail -n1)"
  fi
  configured="${configured:-apps.json}"
  if [[ "${configured}" == /* ]]; then
    printf '%s\n' "${configured}"
  else
    printf '%s\n' "${HOME}/.config/sunshine/${configured}"
  fi
}
migrate_existing_apps() {
  local helper="${PREFIX}/share/steamshine/current/scripts/migrate-steamos-apps.py" apps_file
  apps_file="$(configured_apps_file)"
  if "${DRY_RUN}"; then say "[dry-run] migrate existing applications in ${apps_file}"; return; fi
  [[ -x "${helper}" ]] || return 0
  "${helper}" "${apps_file}" || die 'Existing Sunshine applications could not be migrated safely.' "${EXIT_CONFIG}"
}
install() {
  install_artifact
  configure
  configure_recommended
  migrate_existing_apps
  if "${NO_SERVICE}"; then
    say 'SteamShine is installed; the systemd user service was not changed'
    return
  fi
  install_gamescope_session_guard
  install_service
  enable_service
  if "${NO_START}"; then
    verify_service false
    say "SteamShine autostart: enabled; active=$(service_active && printf true || printf false)"
    say 'SteamShine is installed; it will start at the next user-manager default.target activation'
    return
  fi
  import_desktop_environment
  if service_active; then
    run systemctl --user restart "${SERVICE_UNIT}" || die 'The active SteamShine user service could not be restarted.' "${EXIT_SERVICE}"
  else
    run systemctl --user start "${SERVICE_UNIT}" || die 'The SteamShine user service could not be started.' "${EXIT_SERVICE}"
  fi
  verify_service true
  say "SteamShine autostart: enabled; active=true; MainPID=$(service_main_pid)"
  say 'SteamShine is installed and will start automatically in Game Mode'
}
virtual_display_enabled() { [[ -r "${CONFIG_FILE}" ]] && grep -Eq '^steamos_virtual_display_enabled[[:space:]]*=[[:space:]]*true[[:space:]]*$' "${CONFIG_FILE}"; }
import_desktop_environment() {
  local -a names=(XDG_RUNTIME_DIR WAYLAND_DISPLAY DISPLAY GAMESCOPE_WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS PIPEWIRE_REMOTE) values=() name
  for name in "${names[@]}"; do
    [[ -n "${!name:-}" ]] && values+=("${name}")
  done
  ((${#values[@]})) || return 0
  run systemctl --user import-environment "${values[@]}"
}
start() {
  "${NO_SERVICE}" && die 'start requires the user service.' "${EXIT_SERVICE}"
  if virtual_display_enabled; then compatibility_check; fi
  import_desktop_environment
  enable_service
  if service_active; then
    verify_service true
    say 'Already running; autostart is enabled'
    return
  fi
  run systemctl --user start "${SERVICE_UNIT}" || die 'Service failed to start.' "${EXIT_SERVICE}"
  verify_service true
  say 'SteamShine started; autostart is enabled'
}
stop() { run systemctl --user stop "${SERVICE_UNIT}"; }
restart() {
  "${NO_SERVICE}" && die 'restart requires the user service.' "${EXIT_SERVICE}"
  if virtual_display_enabled; then compatibility_check; fi
  import_desktop_environment
  enable_service
  run systemctl --user restart "${SERVICE_UNIT}" || die 'Service failed to restart.' "${EXIT_SERVICE}"
  verify_service true
  say 'SteamShine restarted; autostart is enabled'
}
status() { systemctl --user status "${SERVICE_UNIT}" --no-pager; }
logs() { journalctl --user -u "${SERVICE_UNIT}" --no-pager -n 200; }
autostart_status() {
  local unit wants runtime load_state unit_file_state active_state sub_state main_pid exec_start executable_realpath build_commit
  local login_state linger_state journal_result service_environment launch_mode=unknown pid
  local -a reasons=() manual_pids=()
  unit="$(service_file)"
  wants="$(service_wants_link)"
  runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  if ! systemctl --user show-environment >/dev/null 2>&1; then
    reasons+=(user_manager_unavailable)
  fi
  load_state="$(systemctl_value LoadState)"
  unit_file_state="$(systemctl --user is-enabled "${SERVICE_UNIT}" 2>/dev/null || true)"
  active_state="$(systemctl_value ActiveState)"
  sub_state="$(systemctl_value SubState)"
  main_pid="$(service_main_pid)"
  exec_start="$(systemctl_value ExecStart)"
  service_environment="$(systemctl_value Environment)"
  if [[ "${service_environment}" =~ (^|[[:space:]])STEAMSHINE_LAUNCH_MODE=([^[:space:]]+) ]]; then
    launch_mode="${BASH_REMATCH[2]}"
  fi
  executable_realpath="$(service_process_executable "${main_pid}" || true)"
  build_commit="$(json_value commit "${PREFIX}/share/steamshine/current/BUILD_INFO.json" 2>/dev/null || true)"
  login_state="$(loginctl show-user "$(id -u)" --property=State --value 2>/dev/null || true)"
  linger_state="$(loginctl show-user "$(id -u)" --property=Linger --value 2>/dev/null || true)"
  journal_result="$(journalctl --user --boot -u "${SERVICE_UNIT}" --no-pager -n 1 -o cat 2>/dev/null | tail -n 1 || true)"
  [[ -f "${unit}" ]] || reasons+=(unit_missing)
  [[ "${unit_file_state}" == enabled ]] || reasons+=(unit_not_enabled)
  [[ -L "${wants}" ]] || reasons+=(wants_symlink_missing)
  [[ -x "${PREFIX}/bin/steamshine" ]] || reasons+=(executable_missing)
  [[ -f "${CONFIG_FILE}" ]] || reasons+=(config_missing)
  [[ "${active_state}" != failed ]] || reasons+=(service_failed)
  if [[ "${journal_result}" == *'start-limit-hit'* || "${journal_result}" == *'Start request repeated too quickly'* ]]; then
    reasons+=(service_start_limited)
  fi
  if [[ "${main_pid}" =~ ^[1-9][0-9]*$ && -x "${PREFIX}/bin/steamshine" ]]; then
    [[ -n "${executable_realpath}" && "${executable_realpath}" == "$(readlink -f -- "${PREFIX}/bin/steamshine")" ]] || reasons+=(wrong_binary)
  fi
  [[ -z "${exec_start}" || "${exec_start}" == *"${PREFIX}/bin/steamshine"* && "${exec_start}" == *"${CONFIG_FILE}"* ]] || reasons+=(wrong_binary)
  if command -v pgrep >/dev/null; then
    while IFS= read -r pid; do
      [[ "${pid}" =~ ^[1-9][0-9]*$ ]] || continue
      [[ "${pid}" == "${main_pid}" ]] || manual_pids+=("${pid}")
    done < <(pgrep -x steamshine 2>/dev/null || true)
  fi
  ((${#manual_pids[@]} == 0)) || reasons+=(manual_process_conflict)
  printf 'service_file=%s\n' "${unit}"
  printf 'unit_load_state=%s\n' "${load_state:-unknown}"
  printf 'unit_file_state=%s\n' "${unit_file_state:-unknown}"
  printf 'active_state=%s\n' "${active_state:-unknown}"
  printf 'sub_state=%s\n' "${sub_state:-unknown}"
  printf 'MainPID=%s\n' "${main_pid:-0}"
  printf 'ExecStart=%s\n' "${exec_start:-unknown}"
  printf 'executable_realpath=%s\n' "${executable_realpath:-unknown}"
  printf 'config_path=%s\n' "${CONFIG_FILE}"
  printf 'BUILD_INFO_commit=%s\n' "${build_commit:-unknown}"
  printf 'default_target_wants_symlink=%s\n' "${wants}"
  printf 'default_target_wants_symlink_state=%s\n' "$([[ -L "${wants}" ]] && printf present || printf missing)"
  printf 'XDG_RUNTIME_DIR=%s\n' "${runtime}"
  printf 'launch_mode=%s\n' "${launch_mode}"
  printf 'loginctl_user_state=%s\n' "${login_state:-unknown}"
  printf 'linger_state=%s\n' "${linger_state:-unknown}"
  printf 'latest_boot_journal_result=%s\n' "${journal_result:-none}"
  printf 'manual_process_pids=%s\n' "${manual_pids[*]:-none}"
  if ((${#reasons[@]})); then
    printf 'autostart_health=failed\n'
    printf 'failure_reasons=%s\n' "${reasons[*]}"
    return 1
  fi
  printf 'autostart_health=healthy\n'
  printf 'failure_reasons=none\n'
}
diagnose() { check; command -v gamescope >/dev/null && gamescope --version || true; pw-cli info 0 >/dev/null 2>&1 && say 'PipeWire reachable' || say 'PipeWire is not reachable'; }
vaapi_amd_driver_status() {
  local vaapi_path
  local -a vaapi_roots=(/usr/lib/dri /usr/lib64/dri /run/host/usr/lib/dri /run/host/usr/lib64/dri)
  if [[ -n "${STEAMSHINE_DRI_ROOTS:-}" ]]; then IFS=: read -r -a vaapi_roots <<<"${STEAMSHINE_DRI_ROOTS}"; fi
  for vaapi_path in "${vaapi_roots[@]}"/radeonsi_drv_video.so; do
    [[ -f "${vaapi_path}" ]] && { say 'VAAPI_AMD_DRIVER_AVAILABLE'; return 0; }
  done
  say 'VAAPI_AMD_DRIVER_MISSING'
}
compatibility_check() {
  check
  local artifact_root="${PREFIX}/share/steamshine/current"
  local baseline="${artifact_root}/STEAMOS_BASELINE.json"
  local collector="${artifact_root}/scripts/collect-steamos-runtime-baseline.sh"
  [[ -f "${baseline}" ]] || baseline="${ROOT_DIR}/ci/steamos/baselines/steamos-3.8.16-20260716.1.json"
  [[ -x "${collector}" ]] || collector="${ROOT_DIR}/scripts/collect-steamos-runtime-baseline.sh"
  local expected_version expected_build expected_glibcxx expected_bdf expected_render actual_glibc actual_glibcxx actual_render
  expected_version="$(json_value version_id "${baseline}")"; expected_build="$(json_value build_id "${baseline}")"; expected_glibcxx="$(json_value max_glibcxx "${baseline}")"
  expected_bdf="$(json_value pci_bdf "${baseline}")"; expected_render="$(json_value render_node "${baseline}")"
  load_os_release
  [[ "${VERSION_ID:-}" == "${expected_version}" ]] || die "SteamOS VERSION_ID ${VERSION_ID:-unknown} is incompatible with artifact baseline ${expected_version}." "$EXIT_UNSUPPORTED"
  [[ "${BUILD_ID:-}" == "${expected_build}" ]] || die "SteamOS BUILD_ID ${BUILD_ID:-unknown} is incompatible with artifact baseline ${expected_build}." "$EXIT_UNSUPPORTED"
  actual_glibc="$(ldd --version | awk 'NR == 1 { for (i = NF; i > 0; --i) if ($i ~ /^[0-9]+\.[0-9]+/) { version = $i; break } } END { print version }')"
  version_at_least "${actual_glibc}" "$(json_value max_glibc "${baseline}")" || die "Host glibc ${actual_glibc:-unknown} is older than the required ABI baseline." "$EXIT_UNSUPPORTED"
  actual_glibcxx="$(strings /usr/lib/libstdc++.so.6 2>/dev/null | grep -E '^GLIBCXX_[0-9]+(\.[0-9]+)+$' | sort -V | tail -1 || true)"
  version_at_least "${actual_glibcxx#GLIBCXX_}" "${expected_glibcxx}" || die "Host ${actual_glibcxx:-unknown} is older than required GLIBCXX_${expected_glibcxx}." "$EXIT_UNSUPPORTED"
  command -v gamescope >/dev/null || die 'Gamescope is required for a SteamOS virtual display.' "$EXIT_DEPENDENCY"
  gamescope --help 2>&1 | grep -- '--backend' >/dev/null || die 'Gamescope lacks --backend required for headless operation.' "$EXIT_DEPENDENCY"
  gamescope --help 2>&1 | grep -- '--prefer-vk-device' >/dev/null || die 'Gamescope lacks deterministic Vulkan device selection.' "$EXIT_DEPENDENCY"
  actual_render="$(readlink -f -- "/dev/dri/by-path/pci-${expected_bdf}-render" 2>/dev/null || true)"
  [[ "${actual_render}" == "${expected_render}" && -r "${actual_render}" && -w "${actual_render}" ]] || die "Expected AMD render node ${expected_render} for ${expected_bdf} is unavailable." "$EXIT_DEPENDENCY"
  say 'GPU_DRM_AVAILABLE'
  say 'VULKAN_RENDER_AVAILABLE'
  if command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep 'VK_KHR_video_encode_h264' >/dev/null; then
    say 'VULKAN_VIDEO_ENCODE_AVAILABLE'
  else
    say 'VULKAN_VIDEO_ENCODE_AVAILABLE=unverified; SteamShine will perform the authoritative Vulkan Video encoder probe at stream start.'
  fi
  if command -v vainfo >/dev/null; then
    say 'VAAPI_PROBE_TOOL_AVAILABLE'
  else
    say 'VAAPI_PROBE_TOOL_MISSING'
  fi
  vaapi_amd_driver_status
  "${collector}"
}
bootstrap() { install; "${DRY_RUN}" || diagnose; say 'SteamShine is ready'; }
update() {
  local was_active=false
  git -C "${ROOT_DIR}" diff --quiet || die 'Uncommitted changes detected; update refused.'
  "${NO_SERVICE}" || { service_active && was_active=true || true; }
  run git -C "${ROOT_DIR}" fetch --all --prune
  run git -C "${ROOT_DIR}" pull --ff-only
  install_artifact
  configure
  configure_recommended
  migrate_existing_apps
  if "${NO_SERVICE}"; then
    say 'SteamShine was updated; the systemd user service was not changed'
    return
  fi
  install_service
  install_gamescope_session_guard
  enable_service
  if "${was_active}" && ! "${NO_START}"; then
    run systemctl --user restart "${SERVICE_UNIT}" || die 'SteamShine was updated but its active service could not be restarted.' "${EXIT_SERVICE}"
    verify_service true
    say 'SteamShine was updated and the active service was restarted'
  else
    verify_service false
    say 'SteamShine was updated; its inactive/running state was preserved and autostart is enabled'
  fi
}
repair() {
  local active_before=false main_pid process_executable expected_executable restart_required=false
  if [[ ! -x "${PREFIX}/bin/steamshine" ]]; then
    install
    return
  fi
  configure
  configure_recommended
  migrate_existing_apps
  "${NO_SERVICE}" && { say 'SteamShine files were repaired; the systemd user service was not changed'; return; }
  service_active && active_before=true || true
  if "${active_before}"; then
    main_pid="$(service_main_pid)"
    process_executable="$(service_process_executable "${main_pid}" || true)"
    expected_executable="$(readlink -f -- "${PREFIX}/bin/steamshine")"
    [[ -n "${process_executable}" && "${process_executable}" == "${expected_executable}" ]] || restart_required=true
  fi
  install_service
  install_gamescope_session_guard
  enable_service
  if "${NO_START}"; then
    verify_service false
  elif "${active_before}"; then
    if "${restart_required}"; then
      run systemctl --user restart "${SERVICE_UNIT}" || die 'SteamShine repair could not restart the stale service process.' "${EXIT_SERVICE}"
    fi
    verify_service true
  else
    run systemctl --user start "${SERVICE_UNIT}" || die 'SteamShine repair could not start the service.' "${EXIT_SERVICE}"
    verify_service true
  fi
  say 'SteamShine autostart was repaired'
}
uninstall() {
  if "${PURGE}" && "${NON_INTERACTIVE}" && ! "${ASSUME_YES}"; then die '--purge in non-interactive mode requires --yes.' "$EXIT_USAGE"; fi
  if ! "${NO_SERVICE}"; then
    run systemctl --user disable --now "${SERVICE_UNIT}" || true
    run rm -f "$(service_file)" "$(service_wants_link)" "$(gamescope_guard_dropin)" "$(gamescope_guard_executable)"
    run systemctl --user daemon-reload || true
    run systemctl --user reset-failed "${SERVICE_UNIT}" || true
  fi
  run rm -f "${PREFIX}/bin/steamshine" "${PREFIX}/bin/steamshine-input-visualizer"
  run rm -rf -- "${PREFIX}/share/steamshine" "${HOME}/.cache/steamshine" "${BUILD_DIR}" "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/steamshine"
  if "${PURGE}"; then run rm -rf -- "${HOME}/.config/steamshine" "${STATE_DIR}"; fi
  if "${REMOVE_DEPENDENCIES}"; then say 'Dependencies are intentionally not removed automatically; inspect installed-packages.txt and remove only packages not used elsewhere.'; fi
}
rollback() {
  local target="${PREFIX}/share/steamshine" versions="${PREFIX}/share/steamshine/versions" previous
  [[ -f "${target}/rollback" ]] || die 'No rollback snapshot is available.' "$EXIT_CONFIG"
  previous="$(<"${target}/rollback")"
  [[ -d "${previous}" ]] && path_within "${previous}" "${versions}" || die 'Rollback target is unsafe or unavailable.' "$EXIT_CONFIG"
  if ! "${DRY_RUN}"; then
    ln -s "${previous}" "${target}/current.next"
    mv -Tf "${target}/current.next" "${target}/current"
    ln -sfn "${target}/current/bin/steamshine" "${PREFIX}/bin/steamshine"
    if [[ -x "${target}/current/bin/steamshine-input-visualizer" ]]; then
      ln -sfn "${target}/current/bin/steamshine-input-visualizer" "${PREFIX}/bin/steamshine-input-visualizer"
    else
      rm -f "${PREFIX}/bin/steamshine-input-visualizer"
    fi
  fi
  say 'Rolled back SteamShine artifact'
}
hardware_test() {
  "${HARDWARE_INTERACTIVE}" || die 'hardware-test requires --interactive because video, audio, and input require operator confirmation.' "$EXIT_USAGE"
  local report_dir="${STATE_DIR}/hardware-tests/$(date +%Y%m%d-%H%M%S)"
  mkdir -p "${report_dir}"
  compatibility_check >"${report_dir}/compatibility.log" 2>&1 || die "Hardware-test compatibility gate failed; see ${report_dir}/compatibility.log" "$EXIT_DEPENDENCY"
  "${ROOT_DIR}/scripts/diagnose-steamos-virtual-display.sh" >"${report_dir}/diagnose.log" 2>&1 || true
  start
  if ! STEAMSHINE_CONFIG="${CONFIG_FILE}" STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${ROOT_DIR}/scripts/test-steamos-virtual-display.sh"; then
    stop || true
    die "Hardware-test failed; SteamShine was stopped for safety but autostart remains enabled. Restart with: systemctl --user start steamshine. See ${report_dir}" "$EXIT_TEST"
  fi
  say "Hardware-test report: ${report_dir}"
}
menu() { while true; do cat <<'EOF'
1) Check environment  2) Install packages  3) Build  4) Configure  5) Bootstrap
6) Start  7) Stop  8) Status  9) Logs  10) Repair  11) Update  12) Uninstall  13) Purge  0) Exit
EOF
read -r -p '> ' choice; case "$choice" in 1) check;; 2) install_packages;; 3) build;; 4) configure;; 5) bootstrap;; 6) start;; 7) stop;; 8) status;; 9) logs;; 10) repair;; 11) update;; 12) uninstall;; 13) PURGE=true; uninstall;; 0) return;; *) say 'Invalid selection';; esac; done; }
main() { require_bash; parse "$@"; if [[ -z "${COMMAND}" ]]; then [[ -t 0 && -t 1 ]] || { usage; exit "$EXIT_USAGE"; }; menu; return; fi; case "${COMMAND}" in menu) menu;; check) check;; compatibility-check) compatibility_check;; vaapi-driver-status) vaapi_amd_driver_status;; install) install;; build) build;; configure) configure;; start) start;; stop) stop;; restart) restart;; status) status;; logs) logs;; diagnose) diagnose;; autostart-status) autostart_status;; update) update;; repair) repair;; uninstall) uninstall;; bootstrap) bootstrap;; rollback) rollback;; hardware-test) hardware_test;; *) usage; exit "$EXIT_USAGE";; esac; }
main "$@"
