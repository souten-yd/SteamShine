<div align="center">
  <img
    src="steamshine.png"
    alt="SteamShine icon"
    width="180"
/>
  <h1 align="center">SteamShine</h1>
  <h4 align="center">A self-hosted Moonlight game-stream host, purpose-built for SteamOS and Steam Deck.</h4>
</div>

## ℹ️ About

SteamShine is a SteamOS/Steam Deck-focused fork of [Sunshine](https://github.com/LizardByte/Sunshine): a
self-hosted, low-latency game-stream host compatible with any [Moonlight](https://moonlight-stream.org/) client.
It adds a SteamOS-native session pipeline (headless virtual-display Gamescope sessions, capture/display selection
tuned for the Deck's AMD APU, and safe coexistence with an already-running Steam Game Mode session), and it ships
its own web control panel — **SteamShine** — in addition to the original Sunshine Web UI.

SteamShine is developed independently at [souten-yd/SteamShine](https://github.com/souten-yd/SteamShine) and does
not currently track upstream Sunshine releases; see [Relationship to Sunshine](#-relationship-to-sunshine) below.

## 🖥️ The SteamShine web UI

Alongside the original Sunshine Web UI (still available at `/`), SteamShine ships a second, purpose-built
control panel at `/steamshine/`:

- **Dashboard** — service, streaming, and SteamOS virtual-display status at a glance.
- **Monitor** — live CPU, RAM, GPU, and VRAM usage with rolling sparklines, per-core CPU bars, and
  temperature/fan/power readouts for AMD GPUs.
- **Applications** — manage the app list Moonlight clients can launch, and stop the currently running one.
- **GPU** — AMD GPU/CPU performance profiles: built-in Silent / Balanced / Performance / OC presets, plus
  fully custom profiles (power limit, CPU governor and clock, GPU clock/voltage offset where the driver
  exposes it) that can be added, renamed, and deleted.
- **Settings** — the most commonly changed configuration options, plus the SteamOS virtual-display policy.
- **Pin** — Moonlight client pairing.
- **Clients** — paired-client list and revocation.
- **Terminal** — a real shell on the host, running as the same unprivileged user as the SteamShine service.
- **Logs** — a bounded diagnostic log view.

This panel uses its own session/CSRF scheme and can be toggled independently via the `steamshine_web_ui_enabled`
and `steamshine_web_ui_default` configuration options.

**Security note:** the GPU profile feature briefly elevates a single Linux capability (`CAP_DAC_OVERRIDE`) around
each sysfs write it makes, immediately dropping it again afterward — see `src/steamshine_gpuctl.cpp`. The
Terminal grants a real, authenticated shell on the host, equivalent in capability to SSH; it sits behind the same
session-cookie + CSRF protections as every other mutating SteamShine endpoint.

## 🚀 Getting started

The supported way to build, install, and run SteamShine on SteamOS is the bundled `steamshine.sh` script:

```bash
./steamshine.sh install   # first-time build + systemd user service setup
./steamshine.sh start
./steamshine.sh status
./steamshine.sh logs
```

Run `./steamshine.sh --help` for the full command list (`build`, `configure`, `stop`, `restart`, `update`,
`repair`, `uninstall`, `hardware-test`, and more). See [docs/STEAMOS_SETUP_SCRIPT.md](docs/STEAMOS_SETUP_SCRIPT.md)
and [docs/INSTALLATION.md](docs/INSTALLATION.md) for details, and
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) if something doesn't come up cleanly.

## 🔀 Relationship to Sunshine

SteamShine started as a fork of Sunshine and reuses large parts of its streaming, pairing, and configuration
stack, but this repository does not currently track an `upstream` remote — it is developed as an independent
project. If you want to pull in a specific upstream Sunshine change yourself, be aware that shared files this
fork has heavily modified (`src/confighttp.cpp`, `src/config.cpp`/`.h`, `src/platform/linux/misc.cpp`, the
Linux packaging scripts) are the most likely to conflict; SteamOS-specific files
(`src/steamos_virtual_session*`, `src/steamshine_*`, the `src_assets/common/assets/steamshine/` web UI) don't
exist upstream and won't conflict at all.

## 🎮 Feature compatibility

SteamShine targets Linux/SteamOS first. For the general gamepad-emulation, HDR, and hardware-requirement
compatibility tables that also apply here, see the equivalent tables in
[upstream Sunshine's README](https://github.com/LizardByte/Sunshine#readme) — this fork hasn't diverged from
that baseline for the parts it still shares.

## 📜 License

SteamShine is distributed under the GPL-3.0 license, the same as upstream Sunshine. See [LICENSE](LICENSE).
