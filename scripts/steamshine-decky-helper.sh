#!/usr/bin/env bash
# @file scripts/steamshine-decky-helper.sh
# @brief Root-owned fixed-operation bridge to the official Decky installer.
set -Eeuo pipefail

readonly action="${1:-}"
readonly caller="${SUDO_USER:-}"
readonly install_url="https://github.com/SteamDeckHomebrew/decky-installer/releases/latest/download/install_release.sh"
readonly uninstall_url="https://github.com/SteamDeckHomebrew/decky-installer/releases/latest/download/uninstall.sh"

## @brief Print immutable input permissions for installation and validation.
## @return Zero after writing the libvirtualhid and rollback-compatible rules.
print_input_rules() {
  cat <<'STEAMSHINE_INPUT_RULES'
# Allows Sunshine to access /dev/uinput
KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput", GROUP="input", MODE="0660", TAG+="uaccess"

# Allows Sunshine to access /dev/uhid
KERNEL=="uhid", GROUP="input", MODE="0660", TAG+="uaccess"

# Joypads
# HID physical and unique identifiers are uevent properties on the hidraw
# node's HID parent, rather than sysfs attributes exposed to ATTRS matching.
SUBSYSTEM=="hidraw", KERNEL=="hidraw*", IMPORT{parent}="HID_*"
SUBSYSTEM=="hidraw", KERNEL=="hidraw*", ENV{HID_PHYS}=="libvirtualhid/uhid/*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{phys}=="libvirtualhid/uhid/*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="hidraw", KERNEL=="hidraw*", ENV{HID_NAME}=="Sunshine (libvirtualhid)*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEMS=="input", ATTRS{name}=="Sunshine (libvirtualhid)*", GROUP="input", MODE="0660", TAG+="uaccess"

# Keep legacy inputtino devices accessible during rollback.
KERNEL=="hidraw*", ATTRS{name}=="Sunshine PS5 (virtual) pad*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEMS=="input", ATTRS{name}=="Sunshine X-Box One (virtual) pad*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEMS=="input", ATTRS{name}=="Sunshine gamepad (virtual) motion sensors*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEMS=="input", ATTRS{name}=="Sunshine Nintendo (virtual) pad*", GROUP="input", MODE="0660", TAG+="uaccess"
SUBSYSTEMS=="input", ATTRS{name}=="Sunshine PS5 (virtual) pad*", GROUP="input", MODE="0660", TAG+="uaccess"
STEAMSHINE_INPUT_RULES
}

# This read-only operation needs no privileged caller or host mutation.
if [[ "${action}" == print-input-rules && $# -eq 1 ]]; then
  print_input_rules
  exit
fi

[[ "${EUID}" -eq 0 ]] || { echo 'Decky helper must run as root.' >&2; exit 77; }
[[ -n "${caller}" && "${caller}" != root ]] || { echo 'Decky helper requires an authenticated non-root caller.' >&2; exit 77; }
[[ $# -eq 1 ]] || { echo 'Usage: steamshine-decky-helper print-input-rules|authorize|configure-input|install|update|uninstall|start|remove-helper' >&2; exit 64; }

case "${action}" in
  authorize)
    exit
    ;;
  configure-input)
    rule_file='/etc/udev/rules.d/60-steamshine.rules'
    temporary_rule="$(mktemp /etc/udev/rules.d/.60-steamshine.rules.XXXXXX)"
    trap 'rm -f -- "${temporary_rule}"' EXIT
    print_input_rules >"${temporary_rule}"
    chmod 0644 "${temporary_rule}"
    chown root:root "${temporary_rule}"
    mv -f -- "${temporary_rule}" "${rule_file}"
    trap - EXIT
    udevadm control --reload-rules
    udevadm trigger --property-match=DEVNAME=/dev/uinput || true
    udevadm trigger --property-match=DEVNAME=/dev/uhid || true
    exit
    ;;
  start)
    [[ -x "/home/${caller}/homebrew/services/PluginLoader" ]] || exit 0
    systemctl start plugin_loader.service
    systemctl is-active --quiet plugin_loader.service
    exit
    ;;
  remove-helper)
    rm -f -- /etc/udev/rules.d/60-steamshine.rules
    udevadm control --reload-rules || true
    udevadm trigger --property-match=DEVNAME=/dev/uinput || true
    udevadm trigger --property-match=DEVNAME=/dev/uhid || true
    rm -f -- /etc/sudoers.d/steamshine-decky
    rm -f -- /var/lib/steamshine/helpers/steamshine-decky-helper
    exit
    ;;
  install|update) script_url="${install_url}" ;;
  uninstall) script_url="${uninstall_url}" ;;
  *) echo 'Unsupported SteamShine privileged-helper action.' >&2; exit 64 ;;
esac

temporary="$(mktemp /var/tmp/steamshine-decky-installer.XXXXXX)"
cleanup() { rm -f -- "${temporary}"; }
trap cleanup EXIT

curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
  --connect-timeout 15 --max-time 120 --max-filesize 1048576 \
  "${script_url}" --output "${temporary}"
[[ -s "${temporary}" ]] || { echo 'Official Decky installer download was empty.' >&2; exit 69; }
head -n 1 "${temporary}" | grep -Eq '^#! */(usr/bin/env +(ba)?sh|bin/(ba)?sh)' || {
  echo 'Official Decky installer had an unexpected format.' >&2
  exit 69
}
chmod 0700 "${temporary}"

# The official CLI scripts use SUDO_USER to locate and retain the invoking
# SteamOS user's homebrew/plugins tree. Running a root-owned temporary copy
# removes the user-writable script race inherent in downloading to HOME.
SUDO_USER="${caller}" USER="${caller}" /usr/bin/bash "${temporary}"
