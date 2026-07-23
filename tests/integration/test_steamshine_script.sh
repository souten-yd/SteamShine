#!/usr/bin/env bash
# @file tests/integration/test_steamshine_script.sh
# @brief Smoke tests for the SteamShine lifecycle command argument boundary.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"

"${root_dir}/steamshine.sh" --help >/dev/null
if "${root_dir}/steamshine.sh" </dev/null >/dev/null 2>&1; then
  echo 'Expected non-TTY invocation without a command to fail.' >&2
  exit 1
fi
if "${root_dir}/steamshine.sh" uninstall --purge --non-interactive --dry-run >/dev/null 2>&1; then
  echo 'Expected non-interactive purge without --yes to fail.' >&2
  exit 1
fi
