#!/usr/bin/env bash
# @file tests/fixtures/steamos/fixture.sh
# @brief Isolated filesystem and command roots for SteamOS shell fixtures.

# Create an isolated SteamOS host view for shell tests. Callers may replace any
# exported root after this function returns to model a missing device or a
# partially populated runtime environment.
steamos_fixture_init() {
  local root="$1"
  export PROC_ROOT="${root}/proc"
  export SYS_ROOT="${root}/sys"
  export DRI_ROOT="${root}/dri"
  export STATE_ROOT="${root}/state"
  export COMMAND_PATH="${root}/bin"
  mkdir -p "${PROC_ROOT}" "${SYS_ROOT}" "${DRI_ROOT}" "${STATE_ROOT}" "${COMMAND_PATH}"
}

# Create a deterministic proc I/O counter fixture for one process identifier.
steamos_fixture_write_proc_io() {
  local process_id="$1" write_bytes="$2" syscalls="$3"
  mkdir -p "${PROC_ROOT}/${process_id}"
  printf 'write_bytes: %s\ncancelled_write_bytes: 0\nsyscr: 0\nsyscw: %s\n' "${write_bytes}" "${syscalls}" >"${PROC_ROOT}/${process_id}/io"
}
