<div align="center">
  <img src="steamshine.png" alt="SteamShine" width="160" />
  <h1>SteamShine</h1>
  <p><strong>A game-streaming host for SteamOS that doesn't need a monitor attached.</strong></p>
  <p>
    <img alt="Platform" src="https://img.shields.io/badge/platform-SteamOS%20%2F%20Linux-1b1b1f" />
    <img alt="GPU" src="https://img.shields.io/badge/GPU-AMD-1b1b1f" />
    <img alt="Clients" src="https://img.shields.io/badge/clients-Moonlight%20%2F%20Artemis-1b1b1f" />
    <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-1b1b1f" />
  </p>
</div>

---

## What it is

SteamShine is a self-hosted game-stream host, compatible with [Moonlight](https://moonlight-stream.org/)
and Artemis clients. It's a fork of [Sunshine](https://github.com/LizardByte/Sunshine) reworked around
one specific machine: **a Steam Deck or SteamOS box acting as a headless streaming console.**

If you've tried to use a Deck (or any SteamOS machine) as a stream host, you already know the two
annoyances. Without a monitor or a dummy HDMI plug, there's often nothing to capture. And when there
*is* a display, you stream at *its* resolution rather than your client's — so a tablet gets a
letterboxed Deck screen, and a 4K TV gets an upscaled one.

SteamShine addresses both, and adds a control panel you can drive from the phone or tablet you're
about to play on.

---

## Why you might like it

- **No monitor, no dummy plug.** SteamShine can build the display it streams from, in memory.
- **The session matches your client.** Your handheld asks for 3040×1904 at 90 Hz; that's the display
  the game actually runs on — not a rescale of something else.
- **It won't fight with Game Mode.** If Steam is already running in Game Mode, SteamShine attaches to
  that session and streams it. It never terminates a session it doesn't own.
- **A control panel built for a touchscreen.** Live telemetry, GPU power profiles, pairing, apps, and
  a real shell — in a dark, single-column layout that works on a phone held one-handed.
- **Everything stays in your home directory.** The SteamOS installer writes only under `~/.local` and
  `~/.config`. No `sudo`, no `pacman`, and SteamOS read-only mode is never disabled.
- **It's still Sunshine underneath.** Pairing, transport, encoders, and the original web UI all work
  the way you expect.

---

## The virtual display, in plain terms

This is the heart of the fork, and it's simpler than the name suggests.

A streaming host normally captures whatever is on a real screen. SteamShine can instead start a
**private Gamescope session** — a compositor with no monitor behind it — created at exactly the size
and refresh rate your client asked for. The game runs in there, frames go straight to the GPU encoder
without ever being drawn on a physical panel, and Moonlight receives them.

```
   Moonlight asks for 1920×1080 @ 60
              │
              ▼
   ┌───────────────────────────┐
   │  Private Gamescope        │   created at 1920×1080 @ 60
   │  (headless — no monitor)  │
   └────────────┬──────────────┘
                │  PipeWire / DMA-BUF   (no CPU copy)
                ▼
        AMD hardware encoder            Vulkan Video or VA-API
                │
                ▼
        Moonlight / Artemis
```

You choose the policy; the default is the middle one:

| Mode | What happens |
| --- | --- |
| `off` | Behaves like stock Sunshine. Captures a real display. |
| `auto` | Uses a real display when one is genuinely capturable; otherwise creates the private session. |
| `force` | Always uses a private session, even with a monitor plugged in. Never falls back. |

And you choose where that session comes from:

| Source | What happens |
| --- | --- |
| `auto` | Attach to a verified Game Mode session if there is one, otherwise create a private one. |
| `existing_gamescope` | Only ever attach to the running Game Mode session. |
| `owned_private` | Always create a fresh private session. |

A few behaviours worth knowing, because they're the ones that bite:

- **Attached is not owned.** When SteamShine attaches to your Game Mode session, it never signals that
  process or removes its runtime directory. Only sessions it created itself are ever torn down.
- **A brief disconnect doesn't kill your game.** By default the session is retained so Moonlight's
  resume lands you back where you were. Turn `steamos_keep_session_alive` off if you'd rather it stop.
- **Steam stays a single instance.** If Steam is already running outside the session SteamShine owns,
  launch requests that would start a second copy are refused with an explanation instead of quietly
  creating a mess. SteamShine will never stop your Steam for you.
- **It fails closed, on purpose.** If the private session's socket goes missing mid-stream, capture
  stops. It does not silently reconnect to your desktop and start broadcasting that instead.
- **Optional local mirror.** If a screen *is* attached, an owned session can be mirrored to it, so you
  can see on the Deck what the remote client sees.

---

## The control panel

The original Sunshine web UI is still there and unchanged. SteamShine adds a second one at
`https://<host>:47990/steamshine/`, designed for the device in your hands rather than a desktop browser.

| Page | What it does |
| --- | --- |
| **Monitor** | CPU, RAM, GPU and VRAM tiles with rolling sparklines, per-core bars, load average and uptime, plus GPU hotspot, fan RPM and power draw against its cap. Refreshes every 2 s. |
| **Apps** | The list Moonlight clients can launch — add, edit, delete, and stop whatever is running now. |
| **GPU** | AMD power profiles. Four built-ins (Silent / Balanced / Performance / OC) scaled from what your hardware actually reports, plus custom profiles: power limit, CPU governor and max clock, and GPU clock/voltage offsets where the driver exposes them. |
| **Settings** | The handful of options people actually change, plus a **Virtual display** page for the policies above. |
| **Pin** | Four-digit Moonlight pairing. |
| **Clients** | Paired clients, and revoking them. |
| **Terminal** | A real shell on the host, in the browser, over its own WebSocket. |

Telemetry is read straight from `/proc` and `/sys` — no `amd-smi` or `sensors` subprocess is spawned
every second, and simply having the panel open creates no extra capture or encoder session. The
control plane is deliberately kept out of the video path: if the web server stalls, the stream doesn't.

The panel has its own login, session cookie and CSRF tokens. It can be turned off entirely
(`steamshine_web_ui_enabled`) or promoted to the root URL (`steamshine_web_ui_default`); configuration
validation won't let you disable both UIs and lock yourself out.

**Two features deserve a straight word.** The GPU page writes to root-owned sysfs files, which it does
by raising a single capability (`CAP_DAC_OVERRIDE`) around each individual write and dropping it
immediately after — against an allow-list of paths resolved from real sysfs enumeration, with every
value clamped to the range the hardware reports. The Terminal is a genuine shell running as the same
unprivileged user as the service: convenient, and roughly equivalent to leaving SSH open. Both sit
behind the same authentication as everything else, but decide for yourself whether you want them
reachable on your network.

---

## What comes from Sunshine

The parts that aren't SteamOS-specific are Sunshine's, and they work as they always have: Moonlight
pairing and transport, gamepad emulation, audio, HDR signalling, the configuration store, and the
original web UI. On the SteamOS path, encoding uses **Vulkan Video** (H.264, HEVC, AV1) or **VA-API**
on your AMD GPU, importing DMA-BUF frames directly rather than copying through system memory.

A few streaming-side additions live here too: bounded input, capture, encoder and network queues that
keep the newest work rather than growing a latency backlog; input routed into the Gamescope session
over EIS/libei so it lands in the game and not on your desktop; and a packaged
`Input Latency Visualizer` app for measuring end-to-end responsiveness yourself.

---

## Requirements

- **SteamOS 3.8, or a comparable Arch-based Linux**, on x86_64. SteamOS 3.8.16 is the measured baseline.
- **An AMD GPU.** Capture, encode and the GPU profile page are all built around `amdgpu`.
- **Gamescope** with `--backend headless` and `--prefer-vk-device` (SteamOS 3.8 ships this), and
  **PipeWire**, for the virtual display. Without them the rest still runs.
- **A Moonlight or Artemis client** on whatever you plan to play on.

`./steamshine.sh compatibility-check` verifies all of the above, and is worth running after any
SteamOS update.

---

## Getting started

Everything is driven by one script. Run it with no arguments in a terminal and it opens a menu.

```bash
./steamshine.sh install     # install, apply recommended settings, and start automatically
./steamshine.sh start
./steamshine.sh status
./steamshine.sh logs
```

`install` applies the recommended virtual-display settings (`enabled=true`, display mode `auto`, and
session source `auto`), enables the systemd user service at login, and starts it immediately. It
preserves unrelated Sunshine settings and keeps the original configuration at
`~/.config/steamshine/backups/sunshine.conf.before-recommended-settings`. Repeated installation is
safe. Use `--no-start` to install without enabling or starting the service, or `--no-service` to omit
the user service entirely.

```bash
./steamshine.sh install --channel pr --pr 8 --non-interactive --yes
./steamshine.sh status                         # confirm that the service is running
./steamshine.sh check                          # check the host environment
./steamshine.sh compatibility-check            # check SteamOS and Gamescope compatibility
./steamshine.sh logs                           # show recent service logs
./steamshine.sh restart                        # restart and retain autostart
./steamshine.sh uninstall                      # remove it and disable autostart; keep configuration
```

Then open `https://<host>:47990/steamshine/`, create your login, and pair your client from the **Pin**
page.

On SteamOS the recommended install uses a prebuilt CI artifact, so no compiler or build tooling is
needed on the Deck itself:

```bash
./steamshine.sh install --channel pr --pr 8
```

The installer verifies the archive checksum, rejects unsafe or linked archive entries, and writes only
under `~/.local`, `~/.config/steamshine`, `~/.local/state/steamshine` and `~/.cache/steamshine`.
Repeat installs are idempotent, the previous version is kept for `./steamshine.sh rollback`, and
`uninstall` removes only what it created.

Other useful commands: `check`, `compatibility-check`, `build`, `configure`, `restart`, `diagnose`,
`update`, `repair`, `rollback`, `hardware-test`. Every modifying command accepts `--dry-run`.

### The settings that matter

Set these in your Sunshine config file, or from the panel's **Virtual display** page:

| Key | Default | Meaning |
| --- | --- | --- |
| `steamos_virtual_display_enabled` | `true` from `steamshine.sh install` | Master switch for everything above. The application-level fallback is `false`. |
| `steamos_virtual_display_mode` | `auto` | `off` / `auto` / `force`. |
| `steamos_session_source` | `auto` | `auto` / `existing_gamescope` / `owned_private`. |
| `steamos_local_presentation` | `auto` | Mirror an owned session to an attached screen. |
| `steamos_keep_session_alive` | `true` | Retain an owned session across a disconnect. |
| `steamshine_web_ui_enabled` | `true` | Serve the SteamShine panel. |
| `steamshine_web_ui_default` | `false` | Put the panel at `/` instead of Sunshine's UI. |

GPU selection (`steamos_game_gpu`, `steamos_capture_gpu`, `steamos_encoder_gpu`), the Gamescope path,
timeouts and default display size are configurable too; see
[docs/STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md](docs/STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md)
for the full list. Left blank, GPU selection picks the AMD render node with the most dedicated VRAM,
and refuses to guess when two candidates are ambiguous.

---

## Project status

An honest summary, because it should affect whether you install this today.

SteamShine works. The virtual-display pipeline — headless Gamescope, PipeWire discovery, DMA-BUF
capture, Vulkan encoding, video, audio, touch and mouse input, and session retention across
disconnects — has been validated end to end on real hardware (SteamOS 3.8.16, RX 9070 XT), including
ten cable disconnect/reconnect cycles. CI covers formatting, linting, build, lifecycle tests, packaging
and installer smoke tests on every change.

What is still outstanding, stated plainly:

- Keyboard and gamepad input, fully monitorless operation, and explicit-stop cleanup are pending a
  final hardware acceptance pass.
- Attaching to a **Game Mode** session needs its own acceptance run; the Desktop Mode result doesn't
  substitute for it.
- HDR is a firm release target, not a shipped one. SDR is what's validated today.
- The redesigned control panel is verified in a browser against a running binary, but its GPU *write*
  path and the Terminal WebSocket have not yet been exercised on live hardware.
- Testing has concentrated on one host and one GPU. Other AMD hardware should work; nobody has proven
  it yet.

If you want a mature, broadly-tested host on any OS, install upstream Sunshine — it's excellent. If
you have a SteamOS machine you want to run headless and you don't mind being early, this is built for
exactly that, and reports from other hardware are genuinely useful.

---

## Documentation

| Document | Contents |
| --- | --- |
| [INSTALLATION.md](docs/INSTALLATION.md) | Install paths and artifact verification. |
| [STEAMOS_SETUP_SCRIPT.md](docs/STEAMOS_SETUP_SCRIPT.md) | Every `steamshine.sh` command and exit code. |
| [STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md](docs/STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md) | How the virtual display actually works. |
| [STEAMOS_ADAPTIVE_STREAMING_DESIGN.md](docs/STEAMOS_ADAPTIVE_STREAMING_DESIGN.md) | Client-adaptive resolution and bitrate. |
| [STREAMING_PRIORITY_AND_PERFORMANCE_POLICY.md](docs/STREAMING_PRIORITY_AND_PERFORMANCE_POLICY.md) | Why the control plane never touches the video path. |
| [IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) | Per-feature status, in more detail than above. |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | When it doesn't come up cleanly. |

---

## Relationship to Sunshine

SteamShine began as a fork of [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) and reuses
a great deal of it. It is developed independently at
[souten-yd/SteamShine](https://github.com/souten-yd/SteamShine) and does not currently track upstream
releases.

If you want to pull a specific upstream change in yourself: the files most likely to conflict are the
ones this fork modified heavily (`src/confighttp.cpp`, `src/config.*`, `src/platform/linux/misc.cpp`,
the Linux packaging scripts). The SteamOS-specific files (`src/steamos_virtual_session*`,
`src/steamshine_*`, and the `src_assets/common/assets/steamshine/` panel) don't exist upstream and
won't conflict at all.

Sincere thanks to the Sunshine and Moonlight projects — none of this would exist without them.

## License

GPL-3.0, the same as upstream Sunshine. See [LICENSE](LICENSE).
