# SteamOS artifact release process

The `SteamOS Runtime Build` GitHub Actions workflow builds an x86_64 artifact from the official Arch Linux `base-devel-20250630.0.373922` image and package archive snapshot. That snapshot supplies glibc 2.41, GCC 15.1.1, and Qt 6.9.1, which are the CI ABI ceilings checked before upload. They are not a claim of measured SteamOS-host compatibility: `hardware-test` records the target host's glibc, libstdc++, Qt, and package inventory before a compatibility claim can be made. The disposable container updates only the official `archlinux-keyring` before verifying packages from the fixed archive; it does not weaken package signature validation. `ci/steamos/Containerfile` mirrors this ephemeral build environment for reproduction. It is intentionally limited to the SteamOS delivery path: focused formatting, ShellCheck, workflow validation, the SteamOS virtual-session GTest filter, installer smoke tests, runtime dependency inspection, and packaging.

It does not run Windows, macOS, FreeBSD, Flatpak, AppImage, Docker, or broad upstream Sunshine test matrices for a normal SteamOS pull request. CUDA and NVIDIA dependencies are disabled for this AMD-focused artifact. ROCm is not bundled; when available on the host, it is reported only by the explicit diagnostic command.

## Validation layers

`SteamOS Runtime Build` first classifies a pull request as `docs`, `shell`,
`cpp-core`, `capture-encode`, `packaging`, `build-system`, or `unclassified`.
An unknown or mixed change fails closed to clean full validation. Documentation
and shell-only changes avoid a Sunshine binary build; shell validation runs
ShellCheck, actionlint, and the installer/hardware-fixture integration test.
The standalone `tests/steamos_core` CMake project covers request normalization
and Gamescope argument construction without configuring or linking the Sunshine
runtime. `tests/steamos_lifecycle` independently links only the virtual-session
manager, test-owned configuration/logging globals, and its direct libraries for
the fake-Gamescope lifecycle layer; it does not build `test_sunshine`.

The full layer uses the digest locked in `ci/steamos/image.lock`, configures a
new build directory, and builds the runtime binary once. Its unit, ABI, package,
and installer-smoke steps consume that same binary. It does not restore a CMake
build directory or treat a compiler cache as validation evidence. Each workflow
uploads `ci-timings.json`; compare ten baseline and ten candidate reports with:

```bash
scripts/compare-steamos-ci-timings.sh baseline/*.json -- candidate/*.json
```

The full-validation artifact also contains `build-timings.json`, derived from
the same Ninja log, with compile and link durations kept separate from test and
package timings.

`SteamOS Artifact Promotion` is manual and takes a successful Runtime Build run
ID plus its artifact name. It verifies the source workflow and checksum, then
creates a release from that artifact. It intentionally does not configure,
compile, or package a second binary.

`SteamOS CI Image` is intentionally manual on feature branches and automatic
only when its inputs reach `master`. This prevents every application pull
request from rebuilding the environment. When changing `Containerfile`, the
snapshot mirror, or package set, dispatch that workflow, verify its provenance,
and update `ci/steamos/image.lock` to its returned digest before relying on it
from Runtime Build.

```text
steamshine-steamos-x86_64-<commit>.tar.zst
steamshine-steamos-x86_64-<commit>.tar.zst.sha256
```

The archive contains user-space application files, scripts, a systemd user-service template, licenses, build metadata, and a runtime dependency report. It deliberately does not bundle graphics drivers, Mesa, Vulkan ICDs, VAAPI drivers, or PipeWire daemons. `libminiupnpc.so.21` is the sole bundled general-purpose library because stock SteamOS does not guarantee that ABI; the archive sets only a relative `$ORIGIN/../lib` RPATH and includes its license. The CI build installs PipeWire only because its `libpipewire-0.3` development interface is a required CMake dependency; it does not start PipeWire, Gamescope, Steam, or any GPU workload.

The build emits `ldd -v`, the dynamic section, symbol versions, and ELF version information, then verifies the staged executable against the CI glibc, libstdc++, C++ ABI, and Qt ceilings. A build that references a newer ABI is rejected before it can be uploaded, with each offending version reported in the CI log.

The starting image is already ABI-compatible, so CI does not downgrade or replace its C library at runtime. Package installation and compilation occur only in the disposable container; they never run on a SteamOS host.

SteamOS installation verifies the detached SHA-256 file, archive paths, absence of link entries, and x86_64 artifact metadata before atomically switching `~/.local/share/steamshine/current`. A failed install keeps the prior `current` version; a successful replacement records the prior version for `./steamshine.sh rollback`, followed by `systemctl --user restart steamshine` when the service is enabled.
