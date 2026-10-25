# SteamOS recovery access

SteamShine's browser terminal is part of the SteamShine process. If the
executable cannot load after a SteamOS update, that terminal cannot start.
Keep the host's OpenSSH daemon and Tailscale daemon enabled as separate system
services. They start independently of `steamshine.service` and allow a remote
shell even when SteamShine exits before initialization.

On this host, both `sshd.service` and `tailscaled.service` are enabled and
running. After the SteamOS 3.9.5 update, SSH answered on port 22 at the
Tailscale address while SteamShine failed to load ICU 76. Check that the
recovery path still works after future updates:

```bash
systemctl is-enabled sshd.service tailscaled.service
systemctl is-active sshd.service tailscaled.service
tailscale ip -4
```

From another device connected to the same tailnet, connect with
`ssh deck@<tailscale-ip>`. Use the deck account's existing login credentials.
The Tailscale IP is printed by `tailscale ip -4`; do not hard-code it in a
recovery script. Confirm an interactive login from another device before
depending on this path.

After login, inspect the loader and service first:

```bash
systemctl --user status steamshine.service
journalctl --user -u steamshine.service -b -n 100 --no-pager
ldd ~/.local/bin/steamshine | grep -E 'not found|version .* not found'
```

For a replacement artifact, use the pinned image in `ci/steamos/image.lock`,
package it with `scripts/package-steamos-artifact.sh`, and verify the packaged
binary with host `ldd` before running `./steamshine.sh install --artifact ...`.
The installer keeps the previous version available for rollback and rejects an
artifact whose required host libraries are unavailable.
