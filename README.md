<div align="center">

<img src="steamshine.png" width="112" alt="SteamShine">

# SteamShine

**Turn your SteamOS box into a headless streaming console.**

No monitor. No dummy HDMI plug. No `sudo`.
Close the lid on your Deck, pick it up in Moonlight from the couch, and the game starts in a
display that was created *for the device in your hands* — the right resolution, the right refresh
rate, encoded on the GPU and never drawn on a physical panel.

![SteamOS](https://img.shields.io/badge/SteamOS-3.8-1A9FFF?logo=steamdeck&logoColor=white)
![AMD](https://img.shields.io/badge/GPU-AMD-ED1C24?logo=amd&logoColor=white)
![Clients](https://img.shields.io/badge/clients-Moonlight%20%2F%20Artemis-4C8BF5)
![No sudo](https://img.shields.io/badge/install-no%20sudo-2E9E4F)
![License](https://img.shields.io/badge/license-GPL--3.0-A42E2B)

A fork of [Sunshine](https://github.com/LizardByte/Sunshine), rebuilt around one machine:
**a SteamOS box with nothing plugged into it.**

</div>

---

## ✨ What it does

|  | Stock Sunshine | SteamShine |
|---|---|---|
| **Needs a display or dummy plug** | Yes | **No** — it can build the display in memory |
| **Session resolution** | Whatever the real screen is | **Exactly what your client asked for** |
| **Steam already in Game Mode** | Captures the panel | **Attaches to that session** and streams it |
| **Installing on SteamOS** | Package manager, unlock the filesystem | **`~/.local` only** — no `sudo`, no `pacman`, FS stays read-only |
| **Control from a phone** | Desktop web UI | **Touch-first panel** with live telemetry, GPU profiles, and a shell |
| **Connection tuning** | Fixed bitrate + FEC | **Adaptive bitrate, frame pacing, per-client profiles, loss diagnostics** |

And on top of that —

- 🖥 **A display made on demand** — 640×480 to 7680×4320, 30–240 Hz, exact rational refresh
  (59.94 never silently becomes 60), and **HDR10 end to end** through a headless session
- 📉 **Adaptive bitrate that doesn't oscillate** — bounded cuts, gentle recovery, and the learned
  rate is saved so the *next* session starts where the last one settled
- 📱 **A panel built for the couch** — CPU/GPU/VRAM tiles, live stream stats, apps, pairing,
  AMD power profiles, and a real shell, all at `https://<host>:47990/steamshine/`
- 🔍 **Loss you can actually see** — per-frame FEC accounting from Moonlight, and every teardown
  records its first cause instead of "the stream ended"
- 🎯 **Per-client profiles that create themselves** — geometry, FPS ceiling, codec, HDR and bitrate
  per client *and* per network class (LAN / Wi-Fi / overlay VPN)
- 🔌 **A blip doesn't kill your game** — owned sessions are retained across a disconnect, so
  Moonlight's resume lands you back exactly where you were
- 🧯 **Nothing it doesn't own gets touched** — it will never stop your Steam, and it fails closed
  rather than quietly broadcasting your desktop instead

Everything Sunshine already did well — pairing, transport, gamepad emulation, audio, the original
web UI — is still here and still works the way you expect.

## 🚀 Quick start

### 1. What you need

| | Requirement | Why |
|---|---|---|
| **Required** | **SteamOS 3.8** or comparable Arch-based Linux, x86_64 | 3.8.16 is the measured baseline |
| **Required** | **An AMD GPU** (`amdgpu`) | Capture, encode and the GPU page are all built around it |
| **Required** | A **systemd user session** with `XDG_RUNTIME_DIR` and `/dev/dri` access | SteamShine runs as your user, not as root |
| **Required** | A **Moonlight** or **Artemis** client | Whatever you plan to play on |
| Optional | **Gamescope** + **PipeWire** | The virtual display. SteamOS 3.8 ships both — without them the rest still runs |

```bash
./steamshine.sh compatibility-check   # verifies all of the above — run it after any SteamOS update
```

### 2. Install

```bash
git clone https://github.com/souten-yd/SteamShine.git
cd SteamShine
./steamshine.sh install
```

That single command downloads the verified SteamOS release, checks its SHA-256 against this repo's
latest GitHub Release, installs it under `~/.local`, registers a systemd **user** service, enables
autostart, and starts it. No compiler, no package manager, nothing outside your home directory.

### 3. Play

1. On the phone, tablet or TV you're about to play on, open **`https://<host>:47990/steamshine/`**
   and create your login.
2. Open the **Pin** page, add the host in Moonlight, and type in the four digits it shows you.
3. Pick a game. The display gets built at whatever resolution your client just asked for.

That's it — from here on the service comes up on its own in Game Mode.

### Everyday commands

```bash
./steamshine.sh status      # is it running?
./steamshine.sh logs        # what did it say?
./steamshine.sh diagnose    # something's off — start here
./steamshine.sh restart     # kick it
./steamshine.sh update      # pull the newest verified release
./steamshine.sh rollback    # go back to the previous one
```

Run `./steamshine.sh` with **no arguments** for an interactive menu instead, or `--help` for the
full command list. Every modifying command accepts `--dry-run`.

> **Removing it:** `./steamshine.sh uninstall` takes out the service, binary and build directory but
> keeps your config, pairings and logs. `--purge` removes those too. It only removes what it created.

## 🖥 The virtual display

This is the heart of the fork, and it's simpler than the name suggests.

A streaming host normally captures whatever is on a real screen. SteamShine can instead start a
**private Gamescope session** — a compositor with no monitor behind it — created at exactly the size
and refresh rate your client asked for. The game runs in there, frames go straight to the GPU
encoder without ever touching a physical panel, and Moonlight receives them.

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

Two dials, and the defaults are the ones you want:

| `steamos_virtual_display_mode` | What happens |
|---|---|
| `off` | Stock Sunshine behaviour. Captures a real display. |
| **`auto`** *(default)* | Real display when one is genuinely capturable; otherwise a private session. |
| `force` | Always private, even with a monitor plugged in. Never falls back. |

| `steamos_session_source` | What happens |
|---|---|
| **`auto`** *(default)* | Attach to a verified Game Mode session if there is one, otherwise create a private one. |
| `existing_gamescope` | Only ever attach to the running Game Mode session. |
| `owned_private` | Always create a fresh private session. |

If a screen *is* attached, an owned session can be mirrored to it, so you can watch locally what the
remote client sees.

## 📡 Connection quality

A stream that looks great for ten seconds and then falls apart is worse than one that never wobbles.
This is where most of the recent work went.

- **Adaptive bitrate, without the hunting.** A fixed-memory controller samples every 500 ms and
  decides every 2 s from real client loss and sender-queue pressure. Cuts are bounded (20% / 10%),
  recovery is gentle (5%), and hysteresis plus an IDR/reconnect cooldown keep it from oscillating.
  It retunes the running encoder — no recreation, no stall.
- **It remembers what worked.** The settled rate is written back into that client's profile, so the
  next session starts there instead of ramping from scratch.
- **Nothing queues up behind you.** Input, capture, encoder and network queues are all bounded and
  latest-frame-wins. Under pressure you lose a frame, not half a second of latency.
- **Frames land on a clock.** Rational FPS scheduling against monotonic deadlines with bounded
  duplicate output. A measured 60 FPS session runs ~59 FPS, p99 encode interarrival near 18 ms.
- **You can see the loss, per frame.** Data and parity packets sent versus received, holes before
  the highest sequence number, block index — parsed straight from Moonlight's FEC status reports.
- **When it drops, it tells you why.** Every teardown records its first cause: control ping timeout,
  protocol error, the client never establishing its video or audio endpoint, remote disconnect,
  local cleanup, service shutdown.
- **Codecs are probed, not assumed.** H.264, HEVC and AV1 are advertised only after the host
  actually opens them — and when one is refused, the reason is visible.
- **HDR10 fails closed.** `off` / `auto` / `require`, with the requested dynamic range kept separate
  from the selected bit depth. An unsafe late fallback errors out rather than quietly handing you
  Main10 SDR and calling it HDR.
- **Record what was actually sent.** The Stream page can capture the exact encoded bitstream that
  went to the client, into a capacity you set. No second encoder, no extra load.

Per-client profiles create themselves: the first time a paired client starts a stream it's recorded
with every policy on automatic, so nothing changes until you want it to. Network classes are named
by you — SteamShine never guesses one from an IP address — and a changed client capability
signature always wins over a saved preference.

## 🎛 The control panel

The original Sunshine web UI is untouched. SteamShine adds a second one at
`https://<host>:47990/steamshine/`, designed for the device in your hands rather than a desktop
browser.

| Page | What it does |
|---|---|
| 📊 **Monitor** | CPU, RAM, GPU and VRAM tiles with rolling sparklines, per-core bars, load average and uptime, plus GPU hotspot, fan RPM and power draw against its cap |
| 📡 **Stream** | Bitrate, frame rate, latency and packet-loss tiles; exactly what the client is being sent; how the link is holding up; and recording. Per-client tuning behind the gear icon |
| 🎮 **Apps** | The list Moonlight clients can launch — add, edit, delete, and stop whatever is running now |
| ⚡ **GPU** | AMD power profiles: Silent / Balanced / Performance / OC, scaled from what your hardware reports, plus custom power limit, CPU governor and clock, and GPU clock/voltage offsets |
| 🖥 **Display** | Virtual-display policy: mode, session source, local mirroring, session retention |
| 🔑 **Pin** / **Clients** | Four-digit pairing, and revoking clients you've paired |
| 💻 **Terminal** | A real shell on the host, in the browser, over its own WebSocket |

Everything else stays in Sunshine's own configuration editor, one click away on every page — the
panel deliberately does not mirror settings you'd only change once.

Telemetry is read straight from `/proc` and `/sys`: no `amd-smi` or `sensors` subprocess spawned
every second, and having the panel open creates no extra capture or encoder session. The control
plane is kept out of the video path — **if the web server stalls, the stream doesn't.**

## 🔒 Safe by default

The parts worth knowing before you put this on your network.

- **It never touches what it doesn't own.** When SteamShine attaches to your Game Mode session it
  never signals that process or removes its runtime directory. Only sessions it created are torn
  down. It will never stop your Steam for you, and it refuses launches that would start a second
  Steam instance instead of quietly making a mess.
- **It fails closed.** If a private session's socket disappears mid-stream, capture stops. It does
  not silently reconnect to your desktop and start broadcasting that instead.
- **Your system stays yours.** The installer writes only under `~/.local`, `~/.config/steamshine`,
  `~/.local/state/steamshine` and `~/.cache/steamshine`. No `sudo`, no `pacman`, SteamOS read-only
  mode is never disabled, and login lingering is deliberately not enabled.
- **Releases are verified, not trusted.** `install` fetches the SteamOS x86_64 archive and its
  SHA-256 from this repo's latest GitHub Release, checks the asset shape and checksum, rejects
  unsafe or linked archive entries, and installs through an immutable version store. Repeat installs
  are idempotent and the previous version stays available for `rollback`.
- **The panel is locked down.** Its own login with throttling, session cookies, CSRF tokens and a
  CSP. Config validation won't let you disable both UIs and lock yourself out. Profiles are stored
  owner-readable-only and replaced atomically.

> ⚠️ **Two features deserve a straight word.**
> The **GPU** page writes to root-owned sysfs. It does so by raising a single capability
> (`CAP_DAC_OVERRIDE`) around each individual write and dropping it immediately after, against an
> allow-list resolved from real sysfs enumeration, with every value clamped to the range the
> hardware reports. The **Terminal** is a genuine shell running as the same unprivileged user as the
> service — convenient, and roughly equivalent to leaving SSH open.
> Both sit behind the same authentication as everything else, but decide for yourself whether you
> want them reachable on your network. `steamshine_web_ui_enabled=false` turns the panel off
> entirely.

## ⚙️ Settings that matter

Set these from the panel's **Display** page, or in your Sunshine config file.

| Key | Default | Meaning |
|---|---|---|
| `steamos_virtual_display_enabled` | `true` after `install` | Master switch for everything above |
| `steamos_virtual_display_mode` | `auto` | `off` / `auto` / `force` |
| `steamos_session_source` | `auto` | `auto` / `existing_gamescope` / `owned_private` |
| `steamos_local_presentation` | `auto` | Mirror an owned session to an attached screen |
| `steamos_keep_session_alive` | `true` | Retain an owned session across a disconnect |
| `steamshine_web_ui_enabled` | `true` | Serve the SteamShine panel |
| `steamshine_web_ui_default` | `false` | Put the panel at `/` instead of Sunshine's UI |

GPU selection (`steamos_game_gpu`, `steamos_capture_gpu`, `steamos_encoder_gpu`), the Gamescope
path, timeouts and default display size are configurable too. Left blank, GPU selection picks the
AMD render node with the most dedicated VRAM — and refuses to guess when two candidates are
ambiguous.

## 🏗 What's where

Everything SteamOS-specific lives in files that don't exist upstream, which is why this fork stays
easy to rebase.

```
steamshine.sh                          installer / service manager / diagnostics (one file, no deps)
src/
├── steamos_virtual_session*.cpp       headless Gamescope sessions, PipeWire discovery, capture
├── steamshine_hwmonitor.cpp           /proc + /sys telemetry for the Monitor page
├── steamshine_gpuctl.cpp              AMD power profiles, capability-scoped sysfs writes
├── steamshine_terminal.cpp            the browser shell's WebSocket backend
├── adaptive_bitrate.h                 the rate controller
├── frame_pacing.cpp                   rational FPS scheduling
├── stream_negotiation.h               geometry / codec / HDR negotiation
├── hdr_policy.h  codec_policy.h        the fail-closed gates
└── latency_diagnostics.h              per-frame FEC and teardown-cause accounting
src_assets/common/assets/steamshine/   the touch-first control panel
docs/                                  design docs, runbooks, per-feature status
```

Runtime state stays under `~/.config/steamshine`, `~/.local/state/steamshine` and
`~/.cache/steamshine` — the repo itself is stateless.

## 🧪 Project status

An honest summary, because it should affect whether you install this today.

**SteamShine works.** The virtual-display pipeline — headless Gamescope, PipeWire discovery,
DMA-BUF capture, Vulkan encoding, video, audio, touch and mouse input, and session retention across
disconnects — has been validated end to end on real hardware (SteamOS 3.8.16, RX 9070 XT), including
ten cable disconnect/reconnect cycles. The panel's Stream page and automatic client registration
were accepted on that same hardware. CI covers formatting, linting, build, lifecycle tests,
packaging and installer smoke tests on every change.

Still outstanding, stated plainly:

| Area | Where it stands |
|---|---|
| Keyboard & gamepad input, fully monitorless operation, explicit-stop cleanup | Pending a final hardware acceptance pass |
| Attaching to a **Game Mode** session | Needs its own acceptance run — the Desktop Mode result doesn't substitute |
| Adaptive bitrate | Controlled-loss Ethernet and Wi-Fi runs not done yet; the Stream page reads the controller's state, which isn't the same as having exercised it |
| GPU *write* path, Terminal WebSocket | Not yet exercised on live hardware |
| Other AMD hardware | Should work. Nobody has proven it — testing has concentrated on one host and one GPU |

If you want a mature, broadly-tested host on any OS, install upstream Sunshine — it's excellent. If
you have a SteamOS machine you want to run headless and you don't mind being early, this is built
for exactly that, and reports from other hardware are genuinely useful.

## 📚 Documentation

| Document | Contents |
|---|---|
| [INSTALLATION.md](docs/INSTALLATION.md) | Install paths and artifact verification |
| [STEAMOS_SETUP_SCRIPT.md](docs/STEAMOS_SETUP_SCRIPT.md) | Every `steamshine.sh` command, option and exit code |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | When it doesn't come up cleanly |
| [UNINSTALLATION.md](docs/UNINSTALLATION.md) | What `uninstall` and `--purge` each remove |
| [STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md](docs/STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md) | How the virtual display actually works |
| [STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md](docs/STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md) | Negotiation state, geometry, codecs, HDR, VBR and adaptive bitrate |
| [STREAM_PROFILES_UI.md](docs/STREAM_PROFILES_UI.md) | Stream-profile matching and the four-stage Stream UI |
| [IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) | Per-feature status, in more detail than above |
| [PROJECT_ROADMAP.md](docs/PROJECT_ROADMAP.md) | Feature order, PR boundaries, hardware gates, rollback |

## 🙏 Relationship to Sunshine

SteamShine began as a fork of [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) and
reuses a great deal of it. It's developed independently at
[souten-yd/SteamShine](https://github.com/souten-yd/SteamShine) and does not currently track
upstream releases. Please report SteamShine issues here, not to LizardByte.

Sincere thanks to the Sunshine and Moonlight projects — none of this would exist without them.

## 📄 License

GPL-3.0, the same as upstream Sunshine. See [LICENSE](LICENSE).

---

<div align="center">
<sub>🎮 Nothing plugged in. Everything streaming.</sub>
</div>
