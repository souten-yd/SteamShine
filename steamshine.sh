#!/usr/bin/env bash
# SteamShine SteamOS lifecycle entry point. Run from the repository root.
set -Eeuo pipefail

readonly EXIT_USAGE=2 EXIT_UNSUPPORTED=3 EXIT_DEPENDENCY=4 EXIT_BUILD=6 EXIT_TEST=7 EXIT_SERVICE=8 EXIT_CONFIG=9 EXIT_UNINSTALL=10
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PREFIX="${HOME}/.local"
BUILD_DIR="${ROOT_DIR}/cmake-build-steamos"
CONFIG_FILE="${HOME}/.config/steamshine/sunshine.conf"
STATE_DIR="${HOME}/.local/state/steamshine"
DRY_RUN=false NON_INTERACTIVE=false ASSUME_YES=false VERBOSE=false QUIET=false FORCE=false NO_START=false NO_BUILD=false NO_PACKAGES=false NO_SERVICE=false PURGE=false REMOVE_DEPENDENCIES=false CLEAN=false BUILD_TYPE=Release

say() { "${QUIET}" || printf '%s\n' "$*"; }
die() { printf 'steamshine: %s\n' "$*" >&2; exit "${2:-1}"; }
run() { if "${DRY_RUN}"; then printf '[dry-run]'; printf ' %q' "$@"; printf '\n'; else "$@"; fi; }
usage() { cat <<'EOF'
Usage: ./steamshine.sh <command> [options]
Commands: menu check install build configure start stop restart status logs diagnose update repair uninstall bootstrap rollback
Options: --non-interactive --yes --dry-run --verbose --quiet --force --no-start --no-build --no-packages --no-service --config PATH --prefix PATH --build-dir PATH --log-file PATH --purge --remove-dependencies --clean --debug --release
EOF
}
require_bash() { [[ -n "${BASH_VERSION:-}" ]] || die 'Run this script with bash.' "$EXIT_USAGE"; }
parse() {
  COMMAND="${1:-}"; [[ $# -gt 0 ]] && shift || true
  if [[ "${COMMAND}" == "-h" || "${COMMAND}" == "--help" ]]; then usage; exit 0; fi
  while [[ $# -gt 0 ]]; do case "$1" in
    --non-interactive) NON_INTERACTIVE=true;; --yes) ASSUME_YES=true;; --dry-run) DRY_RUN=true;; --verbose) VERBOSE=true;; --quiet) QUIET=true;; --force) FORCE=true;; --no-start) NO_START=true;; --no-build) NO_BUILD=true;; --no-packages) NO_PACKAGES=true;; --no-service) NO_SERVICE=true;; --purge) PURGE=true;; --remove-dependencies) REMOVE_DEPENDENCIES=true;; --clean) CLEAN=true;; --debug) BUILD_TYPE=Debug;; --release) BUILD_TYPE=Release;;
    --config|--prefix|--build-dir|--log-file) [[ $# -ge 2 ]] || die "Missing value for $1" "$EXIT_USAGE"; case "$1" in --config) CONFIG_FILE="$2";; --prefix) PREFIX="$2";; --build-dir) BUILD_DIR="$2";; esac; shift;;
    -h|--help) usage; exit 0;; *) die "Unknown option: $1" "$EXIT_USAGE";; esac; shift; done
}
is_steamos_or_arch() { [[ -r /etc/os-release ]] && grep -Eqi 'steamos|arch' /etc/os-release; }
check() {
  say '[1/5] Checking supported environment'; is_steamos_or_arch || die 'SteamOS or Arch Linux is required.' "$EXIT_UNSUPPORTED"
  say '[2/5] Checking user runtime directory'; [[ -n "${XDG_RUNTIME_DIR:-}" && -d "${XDG_RUNTIME_DIR}" ]] || die 'XDG_RUNTIME_DIR is required.' "$EXIT_DEPENDENCY"
  say '[3/5] Checking GPU access'; [[ -r /dev/dri/renderD128 || -r /dev/dri/card0 ]] || die 'No accessible DRM device.' "$EXIT_DEPENDENCY"
  say '[4/5] Checking required commands'; command -v cmake >/dev/null && command -v ninja >/dev/null && command -v pkg-config >/dev/null || die 'cmake, ninja, and pkg-config are required.' "$EXIT_DEPENDENCY"
  say '[5/5] Checking virtual-display prerequisites'; command -v gamescope >/dev/null || say 'Gamescope is optional until steamos_virtual_display_enabled=true.'
  say 'Environment check passed'
}
install_packages() {
  command -v pacman >/dev/null || die 'No supported package manager was found; install build dependencies manually.' "$EXIT_DEPENDENCY"
  local packages=(base-devel cmake ninja pkgconf pipewire gamescope libdrm wayland)
  if ! "${ASSUME_YES}" && ! "${NON_INTERACTIVE}"; then read -r -p "Install missing official packages with pacman? [y/N] " answer; [[ "${answer}" =~ ^[Yy]$ ]] || return 0; fi
  run sudo pacman -S --needed "${packages[@]}"
  if ! "${DRY_RUN}"; then mkdir -p "${STATE_DIR}"; printf '%s\n' "${packages[@]}" >"${STATE_DIR}/installed-packages.txt"; fi
}
build() {
  "${CLEAN}" && run cmake -E rm -rf "${BUILD_DIR}"
  run cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DBUILD_TESTS=ON
  run cmake --build "${BUILD_DIR}" --parallel "$(nproc)" || die 'Build failed.' "$EXIT_BUILD"
  [[ -x "${BUILD_DIR}/tests/test_sunshine" ]] && run "${BUILD_DIR}/tests/test_sunshine" || die 'Unit tests failed.' "$EXIT_TEST"
}
configure() {
  local directory="$(dirname -- "${CONFIG_FILE}")"; run mkdir -p "${directory}" "${directory}/backups"
  if [[ -e "${CONFIG_FILE}" ]]; then say 'Already configured; preserving existing configuration.'; return; fi
  if "${DRY_RUN}"; then say "[dry-run] create ${CONFIG_FILE}"; return; fi
  cat >"${CONFIG_FILE}" <<EOF
# SteamShine SteamOS settings. Virtual display is opt-in.
steamos_virtual_display_enabled = false
steamos_virtual_display_mode = auto
steamos_gamescope_path = gamescope
steamos_runtime_directory = ${XDG_RUNTIME_DIR}/steamshine
steamos_startup_timeout_seconds = 15
steamos_shutdown_timeout_seconds = 5
steamos_default_width = 1920
steamos_default_height = 1080
steamos_default_fps = 60
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
install() { "${NO_PACKAGES}" || install_packages; "${NO_BUILD}" || build; run mkdir -p "${PREFIX}/bin" "${PREFIX}/share/steamshine"; run install -m 755 "${BUILD_DIR}/sunshine" "${PREFIX}/bin/steamshine"; configure; "${NO_SERVICE}" || install_service; }
start() { "${NO_SERVICE}" && die 'start requires the user service.' "$EXIT_SERVICE"; systemctl --user is-active --quiet steamshine && { say 'Already running'; return; }; run systemctl --user enable --now steamshine || die 'Service failed to start.' "$EXIT_SERVICE"; }
stop() { run systemctl --user disable --now steamshine; }
status() { systemctl --user status steamshine --no-pager; }
logs() { journalctl --user -u steamshine --no-pager -n 200; }
diagnose() { check; command -v gamescope >/dev/null && gamescope --version || true; pw-cli info 0 >/dev/null 2>&1 && say 'PipeWire reachable' || say 'PipeWire is not reachable'; }
bootstrap() { check; "${NO_PACKAGES}" || install_packages; "${NO_BUILD}" || build; install; "${NO_START}" || "${NO_SERVICE}" || start; diagnose; say 'SteamShine is ready'; }
update() { git -C "${ROOT_DIR}" diff --quiet || die 'Uncommitted changes detected; update refused.'; run git -C "${ROOT_DIR}" fetch --all --prune; run git -C "${ROOT_DIR}" pull --ff-only; install; "${NO_START}" || start; }
repair() { configure; "${NO_SERVICE}" || install_service; [[ -x "${PREFIX}/bin/steamshine" ]] || install; }
uninstall() {
  if "${PURGE}" && "${NON_INTERACTIVE}" && ! "${ASSUME_YES}"; then die '--purge in non-interactive mode requires --yes.' "$EXIT_USAGE"; fi
  stop || true; run rm -f "$(service_file)" "${PREFIX}/bin/steamshine"; run cmake -E rm -rf "${BUILD_DIR}" "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/steamshine"; run systemctl --user daemon-reload || true
  if "${PURGE}"; then run cmake -E rm -rf "${HOME}/.config/steamshine" "${STATE_DIR}"; fi
  if "${REMOVE_DEPENDENCIES}"; then say 'Dependencies are intentionally not removed automatically; inspect installed-packages.txt and remove only packages not used elsewhere.'; fi
}
rollback() { die 'No rollback snapshot is available yet; restore the timestamped backup in ~/.config/steamshine/backups manually.'; }
menu() { while true; do cat <<'EOF'
1) Check environment  2) Install packages  3) Build  4) Configure  5) Bootstrap
6) Start  7) Stop  8) Status  9) Logs  10) Repair  11) Update  12) Uninstall  13) Purge  0) Exit
EOF
read -r -p '> ' choice; case "$choice" in 1) check;; 2) install_packages;; 3) build;; 4) configure;; 5) bootstrap;; 6) start;; 7) stop;; 8) status;; 9) logs;; 10) repair;; 11) update;; 12) uninstall;; 13) PURGE=true; uninstall;; 0) return;; *) say 'Invalid selection';; esac; done; }
main() { require_bash; parse "$@"; if [[ -z "${COMMAND}" ]]; then [[ -t 0 && -t 1 ]] || { usage; exit "$EXIT_USAGE"; }; menu; return; fi; case "${COMMAND}" in menu) menu;; check) check;; install) install;; build) build;; configure) configure;; start) start;; stop) stop;; restart) stop; start;; status) status;; logs) logs;; diagnose) diagnose;; update) update;; repair) repair;; uninstall) uninstall;; bootstrap) bootstrap;; rollback) rollback;; *) usage; exit "$EXIT_USAGE";; esac; }
main "$@"
