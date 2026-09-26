# Web management and storage recovery

The Addon page shows Steam shader-cache locations and registered ext4 data volumes.
It recognizes existing cache symlinks and marks disconnected libraries separately
from caches that need configuration. “Use internal storage” requires Steam and
games to be closed. It copies shader data to the user's home filesystem, keeps the
original directory under a unique backup name, and switches the library's cache
path to the copy. Proton prefixes and saves are never migrated or removed.
Existing custom or broken links require manual review. Copy failures and insufficient
space leave the original cache in place.

“Save mount settings” stores UUID/path mappings in
`/var/lib/steamshine/storage/mounts.json`. Settings can also be discovered from the
SteamOS update archives under `/var/lib/steamos-atomupd/etc_backup` by reading only
their `etc/fstab` entry, without extracting archives. Recovery accepts only known
UUID-based ext4 data mounts below `/run/media`, `/media`, `/mnt`, or `/var/mnt`.
System volumes, disconnected disks, duplicate UUIDs, mismatched filesystems,
symlinked mount paths, and mount locations containing files are rejected.

“Restore saved mount” checks an unmounted filesystem with `e2fsck -fn`, without
repairing it. Only a successful check permits restoration of the original path.
The helper installs a dedicated systemd mount unit, enables it for future boots,
and verifies the resulting UUID/path. The unit has a 30-second job timeout and is
not a required boot dependency. Existing conflicting units and mounts elsewhere
are preserved. SteamOS currently retains custom `.mount` units and enablement links
across atomic updates. Recovery does not format, repartition, erase, or repair disks.

Decky operations, GPU profile activation, and storage changes request administrator
authentication in a Web dialog only when their fixed-operation helper is missing,
outdated, or unauthorized. The HTTPS API requires the existing Web session, same-origin
validation, and its CSRF token; administrator attempts are rate limited separately.
The password is passed to sudo through an anonymous input socket, never through
command arguments, environment variables, a temporary file, or application logs.
The dialog clears its input immediately and does not use browser storage.

Successful authentication installs root-owned fixed-operation helpers and a scoped
sudoers policy. That policy is explicitly retained across SteamOS atomic updates.
No generic shell, Python execution, arbitrary path, or arbitrary command is exposed
by the policy. Subsequent allowed operations do not need the password again while
the helper version and authorization remain valid. GPU capabilities are refreshed
in-process after authorization; applying a profile requires no SteamShine restart.

Validation: `python3 -m unittest tests.integration.test_steamshine_storage_cache`,
the `SteamshineAddonsTest` and GPU-control GTest suites, and
`node tests/web/addons-management.mjs`. The browser test simulates privileged API
responses and never updates Decky, changes GPU settings, or mounts a real disk.
