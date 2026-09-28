#!/usr/bin/env bash
# Verify the stock launcher waits for a live handoff and clears stale leases.
set -Eeuo pipefail

fixture="$(mktemp -d)"
trap 'rm -r -- "${fixture}"' EXIT
mkdir -p "${fixture}/runtime/steamshine" "${fixture}/drm/card0-HDMI-A-1" "${fixture}/proc/42"
printf 'connected\n' >"${fixture}/drm/card0-HDMI-A-1/status"
printf 'test-boot\n' >"${fixture}/boot_id"
{
  printf '42 (fake) S 1'
  for number in {1..17}; do printf ' 0'; done
  printf ' 1234\n'
} >"${fixture}/proc/42/stat"
cat >"${fixture}/vendor" <<'SH'
#!/usr/bin/env bash
printf 'started\n' >"$STEAMSHINE_TEST_VENDOR_MARKER"
SH
chmod +x "${fixture}/vendor"

export XDG_RUNTIME_DIR="${fixture}/runtime"
export STEAMSHINE_DRM_ROOT="${fixture}/drm"
export STEAMSHINE_PROC_ROOT="${fixture}/proc"
export STEAMSHINE_BOOT_ID_PATH="${fixture}/boot_id"
export STEAMSHINE_CONNECTOR_POLL_SECONDS=0.1
export STEAMSHINE_TEST_VENDOR_MARKER="${fixture}/started"
guard="${1:?guard script path required}"
lease="${fixture}/runtime/steamshine/stock-session-handoff.lease"

printf 'version=1\nboot_id=test-boot\nowner_pid=42\nowner_start_time=1234\ngeneration=1\n' >"${lease}"
chmod 600 "${lease}"
timeout 5 bash "${guard}" "${fixture}/vendor" &
guard_pid=$!
sleep 0.3
test ! -e "${STEAMSHINE_TEST_VENDOR_MARKER}"
mv "${lease}" "${fixture}/released.lease"
wait "${guard_pid}"
test -e "${STEAMSHINE_TEST_VENDOR_MARKER}"

mv "${STEAMSHINE_TEST_VENDOR_MARKER}" "${fixture}/first-start"
printf 'version=1\nboot_id=test-boot\nowner_pid=42\nowner_start_time=999\ngeneration=2\n' >"${lease}"
chmod 600 "${lease}"
timeout 5 bash "${guard}" "${fixture}/vendor"
test -e "${STEAMSHINE_TEST_VENDOR_MARKER}"
test ! -e "${lease}"
