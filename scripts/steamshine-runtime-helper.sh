#!/usr/bin/env bash
# @file scripts/steamshine-runtime-helper.sh
# @brief Root-owned, validated sysfs bridge for SteamShine hardware profiles.
set -Eeuo pipefail

readonly action="${1:-}"
readonly caller="${SUDO_USER:-}"

fail() { printf 'steamshine-runtime-helper: %s\n' "$1" >&2; exit "${2:-65}"; }
integer() { [[ "$1" =~ ^-?[0-9]+$ ]]; }
read_integer() { local value; value="$(<"$1")"; integer "${value}" || return 1; printf '%s\n' "${value}"; }

[[ "${EUID}" -eq 0 ]] || fail 'must run as root' 77
[[ -n "${caller}" && "${caller}" != root ]] || fail 'requires an authenticated non-root caller' 77

case "${action}" in
  authorize)
    [[ $# -eq 1 ]] || fail 'authorize takes no arguments' 64
    exit
    ;;
  remove-helper)
    [[ $# -eq 1 ]] || fail 'remove-helper takes no arguments' 64
    rm -f -- /etc/sudoers.d/steamshine-runtime
    rm -f -- /var/lib/steamshine/helpers/steamshine-runtime-helper
    exit
    ;;
  apply-profile)
    [[ $# -eq 7 ]] || fail 'apply-profile requires six validated values' 64
    ;;
  *)
    fail 'unsupported action' 64
    ;;
esac

readonly power_microwatts="$2"
readonly performance_level="$3"
readonly cpu_governor="$4"
readonly cpu_max_khz="$5"
readonly clock_offset_mhz="$6"
readonly voltage_offset_mv="$7"

gpu_device=''
gpu_hwmon=''
for candidate in /sys/class/drm/card*/device; do
  [[ -r "${candidate}/vendor" && "$(<"${candidate}/vendor")" == 0x1002 ]] || continue
  [[ "$(basename -- "$(readlink -f -- "${candidate}/driver")")" == amdgpu ]] || continue
  gpu_device="$(readlink -f -- "${candidate}")"
  for hwmon in "${gpu_device}"/hwmon/hwmon*; do
    [[ -d "${hwmon}" && -r "${hwmon}/name" && "$(<"${hwmon}/name")" == amdgpu ]] || continue
    gpu_hwmon="$(readlink -f -- "${hwmon}")"
    break
  done
  break
done

if [[ "${power_microwatts}" != - ]]; then
  integer "${power_microwatts}" || fail 'invalid power cap'
  [[ -n "${gpu_hwmon}" ]] || fail 'AMD GPU power hwmon is unavailable' 69
  minimum="$(read_integer "${gpu_hwmon}/power1_cap_min")" || fail 'GPU minimum power cap is invalid' 69
  maximum="$(read_integer "${gpu_hwmon}/power1_cap_max")" || fail 'GPU maximum power cap is invalid' 69
  ((power_microwatts >= minimum && power_microwatts <= maximum)) || fail 'power cap is outside the driver range'
  printf '%s\n' "${power_microwatts}" >"${gpu_hwmon}/power1_cap"
fi

if [[ "${performance_level}" != - ]]; then
  [[ "${performance_level}" == auto || "${performance_level}" == manual ]] || fail 'invalid GPU performance level'
  [[ -n "${gpu_device}" && -e "${gpu_device}/power_dpm_force_performance_level" ]] || fail 'GPU performance level control is unavailable' 69
  printf '%s\n' "${performance_level}" >"${gpu_device}/power_dpm_force_performance_level"
  [[ "$(<"${gpu_device}/power_dpm_force_performance_level")" == "${performance_level}" ]] || fail 'applied GPU performance level did not match the request' 74
fi

if [[ "${cpu_governor}" != - || "${cpu_max_khz}" != - ]]; then
  mapfile -t cpu_dirs < <(printf '%s\n' /sys/devices/system/cpu/cpu[0-9]*/cpufreq | sort -V)
  [[ ${#cpu_dirs[@]} -gt 0 && -d "${cpu_dirs[0]}" ]] || fail 'CPU frequency controls are unavailable' 69
  if [[ "${cpu_governor}" != - ]]; then
    available_governors="$(<"${cpu_dirs[0]}/scaling_available_governors")"
    [[ " ${available_governors} " == *" ${cpu_governor} "* ]] || fail 'CPU governor is not offered by the driver'
  fi
  if [[ "${cpu_max_khz}" != - ]]; then
    integer "${cpu_max_khz}" || fail 'invalid CPU maximum frequency'
    cpu_minimum="$(read_integer "${cpu_dirs[0]}/cpuinfo_min_freq")" || fail 'CPU minimum frequency is invalid' 69
    cpu_maximum="$(read_integer "${cpu_dirs[0]}/cpuinfo_max_freq")" || fail 'CPU maximum frequency is invalid' 69
    ((cpu_max_khz >= cpu_minimum && cpu_max_khz <= cpu_maximum)) || fail 'CPU maximum frequency is outside the driver range'
  fi
  for cpu_dir in "${cpu_dirs[@]}"; do
    [[ -d "${cpu_dir}" ]] || continue
    [[ "${cpu_governor}" == - ]] || printf '%s\n' "${cpu_governor}" >"${cpu_dir}/scaling_governor"
    [[ "${cpu_max_khz}" == - ]] || printf '%s\n' "${cpu_max_khz}" >"${cpu_dir}/scaling_max_freq"
    [[ "${cpu_governor}" == - || "$(<"${cpu_dir}/scaling_governor")" == "${cpu_governor}" ]] || fail 'applied CPU governor did not match the request' 74
    [[ "${cpu_max_khz}" == - || "$(<"${cpu_dir}/scaling_max_freq")" == "${cpu_max_khz}" ]] || fail 'applied CPU maximum frequency did not match the request' 74
  done
fi

if [[ "${clock_offset_mhz}" != - || "${voltage_offset_mv}" != - ]]; then
  [[ -n "${gpu_device}" && -e "${gpu_device}/pp_od_clk_voltage" ]] || fail 'GPU overdrive control is unavailable' 69
  if [[ "${clock_offset_mhz}" != - ]]; then
    integer "${clock_offset_mhz}" || fail 'invalid GPU clock offset'
    ((clock_offset_mhz >= -100 && clock_offset_mhz <= 200)) || fail 'GPU clock offset is outside the allowed range'
    printf 's 1 %s\n' "${clock_offset_mhz}" >"${gpu_device}/pp_od_clk_voltage"
  fi
  if [[ "${voltage_offset_mv}" != - ]]; then
    integer "${voltage_offset_mv}" || fail 'invalid GPU voltage offset'
    ((voltage_offset_mv >= -150 && voltage_offset_mv <= 0)) || fail 'GPU voltage offset is outside the allowed range'
    printf 'vo %s\n' "${voltage_offset_mv}" >"${gpu_device}/pp_od_clk_voltage"
  fi
  printf 'c\n' >"${gpu_device}/pp_od_clk_voltage"
fi

# Verify power last so a later performance-level or overdrive operation cannot
# silently reset a limit that was already reported as applied.
if [[ "${power_microwatts}" != - ]]; then
  observed="$(read_integer "${gpu_hwmon}/power1_cap")" || fail 'applied GPU power cap could not be read back' 74
  tolerance=$((power_microwatts / 200))
  ((tolerance >= 1000000)) || tolerance=1000000
  ((observed >= power_microwatts - tolerance && observed <= power_microwatts + tolerance)) || fail 'applied GPU power cap did not match the request' 74
fi
