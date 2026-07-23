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

# A normal tar archive contains a leading ./ entry.  It is safe and must not be
# mistaken for a parent-directory traversal by the immutable artifact installer.
test_root="$(mktemp -d)"
trap 'rm -rf -- "${test_root}"' EXIT
mkdir -p "${test_root}/stage/bin" "${test_root}/home/run"
install -m 755 /bin/true "${test_root}/stage/bin/steamshine"
printf '{}\n' >"${test_root}/stage/BUILD_INFO.json"
tar --zstd -C "${test_root}/stage" -cf "${test_root}/steamshine-steamos-x86_64-test.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-x86_64-test.tar.zst >steamshine-steamos-x86_64-test.tar.zst.sha256)
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-service --non-interactive --yes
test -x "${test_root}/home/.local/bin/steamshine"

# Immutable SteamOS installs must be removable without local development tools.
# The normal uninstall removes only generated binaries/cache/runtime files and
# deliberately preserves user configuration and diagnostic state.
mkdir -p "${test_root}/home/.config/steamshine" "${test_root}/home/.local/state/steamshine"
printf 'keep\n' >"${test_root}/home/.config/steamshine/sunshine.conf"
printf 'keep\n' >"${test_root}/home/.local/state/steamshine/diagnostics.log"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" uninstall --no-service --non-interactive --yes
test ! -e "${test_root}/home/.local/bin/steamshine"
test ! -e "${test_root}/home/.local/share/steamshine/current"
test ! -d "${test_root}/home/.cache/steamshine"
test -f "${test_root}/home/.config/steamshine/sunshine.conf"
test -f "${test_root}/home/.local/state/steamshine/diagnostics.log"
