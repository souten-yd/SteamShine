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
printf '{"target_architecture":"x86_64"}\n' >"${test_root}/stage/BUILD_INFO.json"
printf '{}\n' >"${test_root}/stage/STEAMOS_BASELINE.json"
tar --zstd -C "${test_root}/stage" -cf "${test_root}/steamshine-steamos-x86_64-test.tar.zst" .
(cd "${test_root}" && sha256sum steamshine-steamos-x86_64-test.tar.zst >steamshine-steamos-x86_64-test.tar.zst.sha256)
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" \
  "${root_dir}/steamshine.sh" install --artifact "${test_root}/steamshine-steamos-x86_64-test.tar.zst" --no-service --non-interactive --yes

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
proc_root="${test_root}/proc"
mkdir -p "${proc_root}/101" "${proc_root}/202" "${test_root}/hardware-bin"
printf 'write_bytes: 28672\ncancelled_write_bytes: 0\nsyscw: 4\n' >"${proc_root}/101/io"
printf 'write_bytes: 4096\ncancelled_write_bytes: 0\nsyscw: 2\n' >"${proc_root}/202/io"
cat >"${test_root}/hardware-bin/pgrep" <<'EOF'
#!/usr/bin/env bash
printf '101\n202\n'
EOF
chmod 755 "${test_root}/hardware-bin/pgrep"
hardware_report="${test_root}/hardware-report"
PATH="${test_root}/hardware-bin:${PATH}" PROC_ROOT="${proc_root}" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_report}" \
  "${root_dir}/scripts/test-steamos-ssd-writes.sh" 0
grep -Fq 'write_bytes=32768' "${hardware_report}/ssd-writes.log"
grep -Fq 'delta write_bytes=0' "${hardware_report}/ssd-writes.log"
PATH="${test_root}/hardware-bin:${PATH}" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_report}" \
  "${root_dir}/scripts/test-steamos-latency.sh"
grep -Fq 'pidstat unavailable' "${hardware_report}/latency.log"

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
for _ in $(seq 1 20); do printf '\n'; done >"${acceptance_input}"
for _ in video audio keyboard mouse gamepad; do printf 'y\n'; done >>"${acceptance_input}"
HOME="${test_root}/home" XDG_RUNTIME_DIR="${test_root}/home/run" PATH="${test_root}/hardware-bin:${PATH}" \
  STEAMSHINE_BINARY="${test_root}/hardware-bin/steamshine" STEAMSHINE_HARDWARE_REPORT_DIR="${hardware_acceptance_report}" \
  STEAMSHINE_HARDWARE_SAMPLE_SECONDS=0 "${root_dir}/scripts/test-steamos-virtual-display.sh" <"${acceptance_input}"
grep -Fq '"result": "pass"' "${hardware_acceptance_report}/hardware-report.json"
grep -Fq '"connect_disconnect_cycles": 10' "${hardware_acceptance_report}/hardware-report.json"
