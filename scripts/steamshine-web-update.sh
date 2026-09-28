#!/usr/bin/env bash
# Run a release update outside the SteamShine service and retain its outcome.
set -Eeuo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
status_dir="${HOME}/.local/state/steamshine"
mkdir -p -- "${status_dir}"
status_file="${status_dir}/web-update.status"
printf 'running\n' >"${status_file}"
trap 'printf "failed\n" >"${status_file}"' EXIT
bash "${root_dir}/steamshine.sh" update --latest-release --non-interactive --yes
trap - EXIT
printf 'success\n' >"${status_file}"
