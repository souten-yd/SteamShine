#!/usr/bin/env bash
# SteamShine SteamOS lifecycle entry point. Run from the repository root.
# shellcheck disable=SC1091,SC2015,SC2034,SC2155
set -Eeuo pipefail

readonly EXIT_USAGE=2 EXIT_UNSUPPORTED=3 EXIT_DEPENDENCY=4 EXIT_BUILD=6 EXIT_TEST=7 EXIT_SERVICE=8 EXIT_CONFIG=9 EXIT_UNINSTALL=10
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PREFIX="${HOME}/.local"
BUILD_DIR="${ROOT_DIR}/cmake-build-steamos"
CONFIG_FILE="${HOME}/.config/steamshine/sunshine.conf"
STATE_DIR="${HOME}/.local/state/steamshine"
DRY_RUN=false NON_INTERACTIVE=false ASSUME_YES=false VERBOSE=false QUIET=false FORCE=false NO_START=false NO_BUILD=false NO_PACKAGES=false NO_SERVICE=false PURGE=false REMOVE_DEPENDENCIES=false CLEAN=false HARDWARE_INTERACTIVE=false BUILD_TYPE=Release
GAME_GPU="" CAPTURE_GPU="" ENCODER_GPU="" GAMESCOPE_PATH="gamescope" DEFAULT_WIDTH=1920 DEFAULT_HEIGHT=1080 DEFAULT_FPS=60
CHANNEL="stable" PR_NUMBER="" RELEASE_TAG="" ARTIFACT_PATH=""

say() { "${QUIET}" || printf '%s\n' "$*"; }
die() { printf 'steamshine: %s\n' "$*" >&2; exit "${2:-1}"; }
run() { if "${DRY_RUN}"; then printf '[dry-run]'; printf ' %q' "$@"; printf '\n'; else "$@"; fi; }
usage() { cat <<'EOF'
Usage: ./steamshine.sh <command> [options]
Commands: menu check compatibility-check install build configure start stop restart status logs diagnose update repair uninstall bootstrap rollback hardware-test
Options: --non-interactive --yes --dry-run --verbose --quiet --force --no-start --no-build --no-packages --no-service --config PATH --prefix PATH --build-dir PATH --channel stable|nightly|pr --pr NUMBER --release TAG --artifact PATH --game-gpu ID --capture-gpu ID --encoder-gpu ID --gamescope-path PATH --default-width PX --default-height PX --default-fps FPS --log-file PATH --purge --remove-dependencies --clean --debug --release
EOF
}
require_bash() { [[ -n "${BASH_VERSION:-}" ]] || die 'Run this script with bash.' "$EXIT_USAGE"; }
parse() {
  COMMAND="${1:-}"; [[ $# -gt 0 ]] && shift || true
  if [[ "${COMMAND}" == "-h" || "${COMMAND}" == "--help" ]]; then usage; exit 0; fi
  while [[ $# -gt 0 ]]; do case "$1" in
    --non-interactive) NON_INTERACTIVE=true;; --interactive) HARDWARE_INTERACTIVE=true;; --yes) ASSUME_YES=true;; --dry-run) DRY_RUN=true;; --verbose) VERBOSE=true;; --quiet) QUIET=true;; --force) FORCE=true;; --no-start) NO_START=true;; --no-build) NO_BUILD=true;; --no-packages) NO_PACKAGES=true;; --no-service) NO_SERVICE=true;; --purge) PURGE=true;; --remove-dependencies) REMOVE_DEPENDENCIES=true;; --clean) CLEAN=true;; --debug) BUILD_TYPE=Debug;; --release) BUILD_TYPE=Release;;
    --config|--prefix|--build-dir|--log-file|--channel|--pr|--artifact|--game-gpu|--capture-gpu|--encoder-gpu|--gamescope-path|--default-width|--default-height|--default-fps) [[ $# -ge 2 ]] || die "Missing value for $1" "$EXIT_USAGE"; case "$1" in --config) CONFIG_FILE="$2";; --prefix) PREFIX="$2";; --build-dir) BUILD_DIR="$2";; --channel) CHANNEL="$2";; --pr) PR_NUMBER="$2";; --artifact) ARTIFACT_PATH="$2";; --game-gpu) GAME_GPU="$2";; --capture-gpu) CAPTURE_GPU="$2";; --encoder-gpu) ENCODER_GPU="$2";; --gamescope-path) GAMESCOPE_PATH="$2";; --default-width) DEFAULT_WIDTH="$2";; --default-height) DEFAULT_HEIGHT="$2";; --default-fps) DEFAULT_FPS="$2";; esac; shift;;
    -h|--help) usage; exit 0;; *) die "Unknown option: $1" "$EXIT_USAGE";; esac; shift; done
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
# SteamShine SteamOS settings. Virtual display is opt-in.
steamos_virtual_display_enabled = false
steamos_virtual_display_mode = auto
steamos_gamescope_path = ${GAMESCOPE_PATH}
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
service_file() { printf '%s\n' "${HOME}/.config/systemd/user/steamshine.service"; }
install_service() {
  local unit; unit="$(service_file)"; run mkdir -p "$(dirname -- "${unit}")"
  if "${DRY_RUN}"; then say "[dry-run] create ${unit}"; return; fi
  cat >"${unit}" <<EOF
[Unit]
Description=SteamShine game streaming host
After=graphical-session.target
[Service]
ExecStart=${PREFIX}/bin/steamshine --config ${CONFIG_FILE}
Restart=on-failure
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=3
[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
}
fetch_artifact() {
  [[ -n "${ARTIFACT_PATH}" ]] && return
  command -v gh >/dev/null || die 'Install gh or download the PR artifact from Actions and pass --artifact <path>.' "$EXIT_DEPENDENCY"
  local cache="${HOME}/.cache/steamshine/artifacts" run_id
  run mkdir -p "${cache}"
  if [[ "${CHANNEL}" == pr ]]; then [[ -n "${PR_NUMBER}" ]] || die '--channel pr requires --pr NUMBER.' "$EXIT_USAGE"; run_id="$(gh run list --repo souten-yd/SteamShine --branch "$(gh pr view "${PR_NUMBER}" --json headRefName -q .headRefName)" --workflow build-steamos.yml --limit 1 --json databaseId -q '.[0].databaseId')"; else die 'Use --artifact for local artifacts; release/channel download is not published yet.' "$EXIT_USAGE"; fi
  [[ -n "${run_id}" && "${run_id}" != null ]] || die 'No completed build-steamos artifact was found for this PR.' "$EXIT_DEPENDENCY"
  run gh run download "${run_id}" --repo souten-yd/SteamShine --dir "${cache}"
  ARTIFACT_PATH="$(find "${cache}" -type f -name 'steamshine-steamos-*.tar.zst' -print -quit)"
}
install_artifact() {
  if "${DRY_RUN}"; then say "[dry-run] verify and install artifact ${ARTIFACT_PATH:-for channel ${CHANNEL}} below ${PREFIX}"; return; fi
  fetch_artifact
  [[ -f "${ARTIFACT_PATH}" ]] || die 'A local .tar.zst artifact is required.' "$EXIT_DEPENDENCY"
  [[ "$(uname -m)" == x86_64 ]] || die 'This artifact supports x86_64 only.' "$EXIT_UNSUPPORTED"
  local checksum="${ARTIFACT_PATH}.sha256" target="${PREFIX}/share/steamshine" versions="${PREFIX}/share/steamshine/versions" extract
  [[ -f "${checksum}" ]] || die "Missing checksum: ${checksum}" "$EXIT_DEPENDENCY"
  (cd -- "$(dirname -- "${checksum}")" && sha256sum -c "$(basename -- "${checksum}")") || die 'Artifact checksum mismatch.' "$EXIT_DEPENDENCY"
  tar --zstd -tf "${ARTIFACT_PATH}" | grep -Eq '(^/|(^|/)\.\.(/|$))' && die 'Unsafe archive path rejected.' "$EXIT_DEPENDENCY"
  mkdir -p "${HOME}/.cache/steamshine"; extract="$(mktemp -d "${HOME}/.cache/steamshine/extract.XXXXXX")"
  if ! tar --zstd -C "${extract}" -xf "${ARTIFACT_PATH}"; then rm -rf -- "${extract}"; die 'Artifact extraction failed.' "$EXIT_DEPENDENCY"; fi
  if [[ ! -x "${extract}/bin/steamshine" || ! -f "${extract}/BUILD_INFO.json" ]]; then rm -rf -- "${extract}"; die 'Artifact layout is invalid.' "$EXIT_DEPENDENCY"; fi
  run mkdir -p "${versions}" "${PREFIX}/bin"; local version; version="$(sha256sum "${ARTIFACT_PATH}" | awk '{print $1}')"
  run mv "${extract}" "${versions}/${version}"; run ln -sfn "${versions}/${version}" "${target}/current"; run ln -sfn "${target}/current/bin/steamshine" "${PREFIX}/bin/steamshine"
}
install() { install_artifact; configure; "${NO_SERVICE}" || install_service; }
start() { "${NO_SERVICE}" && die 'start requires the user service.' "$EXIT_SERVICE"; systemctl --user is-active --quiet steamshine && { say 'Already running'; return; }; run systemctl --user enable --now steamshine || die 'Service failed to start.' "$EXIT_SERVICE"; }
stop() { run systemctl --user disable --now steamshine; }
status() { systemctl --user status steamshine --no-pager; }
logs() { journalctl --user -u steamshine --no-pager -n 200; }
diagnose() { check; command -v gamescope >/dev/null && gamescope --version || true; pw-cli info 0 >/dev/null 2>&1 && say 'PipeWire reachable' || say 'PipeWire is not reachable'; }
compatibility_check() {
  check
  local collector="${PREFIX}/share/steamshine/current/scripts/collect-steamos-runtime-baseline.sh"
  [[ -x "${collector}" ]] || collector="${ROOT_DIR}/scripts/collect-steamos-runtime-baseline.sh"
  "${collector}"
}
bootstrap() { install; "${NO_START}" || "${NO_SERVICE}" || start; "${DRY_RUN}" || diagnose; say 'SteamShine is ready'; }
update() { git -C "${ROOT_DIR}" diff --quiet || die 'Uncommitted changes detected; update refused.'; run git -C "${ROOT_DIR}" fetch --all --prune; run git -C "${ROOT_DIR}" pull --ff-only; install; "${NO_START}" || start; }
repair() { configure; "${NO_SERVICE}" || install_service; [[ -x "${PREFIX}/bin/steamshine" ]] || install; }
uninstall() {
  if "${PURGE}" && "${NON_INTERACTIVE}" && ! "${ASSUME_YES}"; then die '--purge in non-interactive mode requires --yes.' "$EXIT_USAGE"; fi
  "${NO_SERVICE}" || stop || true
  run rm -f "$(service_file)" "${PREFIX}/bin/steamshine"
  run rm -rf -- "${PREFIX}/share/steamshine" "${HOME}/.cache/steamshine" "${BUILD_DIR}" "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/steamshine"
  "${NO_SERVICE}" || run systemctl --user daemon-reload || true
  if "${PURGE}"; then run rm -rf -- "${HOME}/.config/steamshine" "${STATE_DIR}"; fi
  if "${REMOVE_DEPENDENCIES}"; then say 'Dependencies are intentionally not removed automatically; inspect installed-packages.txt and remove only packages not used elsewhere.'; fi
}
rollback() { die 'No rollback snapshot is available yet; restore the timestamped backup in ~/.config/steamshine/backups manually.'; }
hardware_test() {
  "${HARDWARE_INTERACTIVE}" || die 'hardware-test requires --interactive because video, audio, and input require operator confirmation.' "$EXIT_USAGE"
  local report_dir="${STATE_DIR}/hardware-tests/$(date +%Y%m%d-%H%M%S)"
  mkdir -p "${report_dir}"
  "${ROOT_DIR}/scripts/diagnose-steamos-virtual-display.sh" >"${report_dir}/diagnose.log" 2>&1 || true
  start
  STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${ROOT_DIR}/scripts/test-steamos-virtual-display.sh"
  say "Hardware-test report: ${report_dir}"
}
menu() { while true; do cat <<'EOF'
1) Check environment  2) Install packages  3) Build  4) Configure  5) Bootstrap
6) Start  7) Stop  8) Status  9) Logs  10) Repair  11) Update  12) Uninstall  13) Purge  0) Exit
EOF
read -r -p '> ' choice; case "$choice" in 1) check;; 2) install_packages;; 3) build;; 4) configure;; 5) bootstrap;; 6) start;; 7) stop;; 8) status;; 9) logs;; 10) repair;; 11) update;; 12) uninstall;; 13) PURGE=true; uninstall;; 0) return;; *) say 'Invalid selection';; esac; done; }
main() { require_bash; parse "$@"; if [[ -z "${COMMAND}" ]]; then [[ -t 0 && -t 1 ]] || { usage; exit "$EXIT_USAGE"; }; menu; return; fi; case "${COMMAND}" in menu) menu;; check) check;; compatibility-check) compatibility_check;; install) install;; build) build;; configure) configure;; start) start;; stop) stop;; restart) stop; start;; status) status;; logs) logs;; diagnose) diagnose;; update) update;; repair) repair;; uninstall) uninstall;; bootstrap) bootstrap;; rollback) rollback;; hardware-test) hardware_test;; *) usage; exit "$EXIT_USAGE";; esac; }
main "$@"
