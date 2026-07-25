/**
 * @file src/steamos_virtual_session.cpp
 * @brief SteamOS headless Gamescope session lifecycle implementation.
 */
#include "steamos_virtual_session.h"

#include "config.h"
#include "logging.h"
#include "rtsp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace steamos_virtual_session {
  namespace {
    struct manager_t {
      std::mutex mutex;  ///< Serializes virtual-session state transitions.
      state_e current {state_e::Disabled};  ///< Current lifecycle state.
      std::filesystem::path runtime_directory;  ///< Runtime path uniquely owned by this process.
      std::string pci_bdf;  ///< PCI BDF of the AMD dGPU selected for Gamescope, capture, and encoding.
      std::string render_node;  ///< AMD dGPU render node shared by Gamescope, capture, and encoders.
      bool stream_requested {false};  ///< Whether RTSP accepted the associated stream before capture attached.
      std::atomic_bool packet_tracking {false};  ///< Whether the video sender may update virtual-session metrics.
      std::atomic_uint64_t encoded_packets {0};  ///< Encoded packets emitted during the owned session.
      std::atomic_uint64_t encoded_bytes {0};  ///< Encoded payload bytes emitted during the owned session.
      std::atomic_uint64_t idr_packets {0};  ///< IDR packets emitted during the owned session.
      std::atomic_uint64_t captured_frames {0};  ///< Wayland DMA-BUF frames acquired for the owned session.
#if defined(__linux__)
      pid_t process_group {-1};  ///< Process group containing Gamescope and its children.
#endif
    } manager;

    constexpr std::string_view owner_marker_name {"steamshine-owner"};
    constexpr std::string_view owner_marker_contents {"steamshine-steamos-virtual-session-v1\n"};
    constexpr std::string_view gamescope_pid_name {"gamescope.pid"};

    /**
     * @brief Check that a path is a UNIX-domain socket.
     *
     * @param path Candidate socket path.
     * @return True only for an existing UNIX-domain socket.
     */
    bool owned_wayland_socket_exists(const std::filesystem::path &path) {
      std::error_code error;
      return std::filesystem::is_socket(std::filesystem::status(path, error)) && !error;
    }

#if defined(__linux__)
    /**
     * @brief Find processes whose runtime environment exactly matches a session directory.
     *
     * @param runtime_directory Marker-owned session directory.
     * @return Process IDs that inherited the owned virtual-session runtime path.
     */
    std::vector<pid_t> processes_using_runtime_directory(const std::filesystem::path &runtime_directory) {
      std::vector<pid_t> processes;
      const std::string needle {"XDG_RUNTIME_DIR=" + runtime_directory.string() + '\0'};
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator {"/proc", error}) {
        if (error) {
          break;
        }
        const auto name {entry.path().filename().string()};
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char character) {
              return std::isdigit(character);
            })) {
          continue;
        }
        std::ifstream environment {entry.path() / "environ", std::ios::binary};
        if (!environment) {
          continue;
        }
        const std::string contents {std::istreambuf_iterator<char> {environment}, {}};
        if (contents.find(needle) == std::string::npos) {
          continue;
        }
        try {
          processes.emplace_back(static_cast<pid_t>(std::stol(name)));
        } catch (const std::exception &) {
        }
      }
      return processes;
    }

    /**
     * @brief Stop processes proven to use an orphaned owned runtime directory.
     *
     * @param runtime_directory Marker-owned runtime directory.
     * @param timeout Maximum graceful wait before force termination.
     */
    void stop_processes_using_runtime_directory(const std::filesystem::path &runtime_directory, const std::chrono::seconds timeout) {
      auto processes {processes_using_runtime_directory(runtime_directory)};
      for (const auto process : processes) {
        ::kill(process, SIGTERM);
      }
      const auto deadline {std::chrono::steady_clock::now() + timeout};
      while (!processes.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
        processes = processes_using_runtime_directory(runtime_directory);
      }
      for (const auto process : processes) {
        ::kill(process, SIGKILL);
      }
    }

    /**
     * @brief Check whether an owned process group still has a live member.
     *
     * @param process_group Process group established by SteamShine before exec.
     * @return True when at least one process in the owned group remains alive.
     */
    bool process_group_exists(const pid_t process_group) {
      return ::kill(-process_group, 0) == 0 || errno == EPERM;
    }

    /**
     * @brief Gracefully stop an owned process group and force-stop remaining children.
     *
     * @param process_group Process group established by SteamShine before exec.
     * @param timeout Maximum graceful-shutdown time.
     */
    void stop_owned_process_group(const pid_t process_group, const std::chrono::seconds timeout) {
      if (process_group <= 0) {
        return;
      }
      ::kill(-process_group, SIGTERM);
      const auto deadline {std::chrono::steady_clock::now() + timeout};
      while (process_group_exists(process_group) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
      }
      if (process_group_exists(process_group)) {
        ::kill(-process_group, SIGKILL);
      }
      ::waitpid(process_group, nullptr, WNOHANG);
    }
#endif

    /**
     * @brief Return a failed manager to a reusable state while holding its mutex.
     *
     * Only the saved process group and per-session runtime directory are
     * touched, so a failed SteamShine launch cannot affect a user's unrelated
     * Gamescope or desktop session.
     */
    void recover_failed_session_locked() {
#if defined(__linux__)
      if (manager.process_group > 0) {
        stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
      }
      manager.process_group = -1;
#endif
      std::error_code error;
      std::filesystem::remove_all(manager.runtime_directory, error);
      manager.runtime_directory.clear();
      manager.pci_bdf.clear();
      manager.render_node.clear();
      manager.stream_requested = false;
      manager.current = config::steamos_virtual_display.enabled ? state_e::Idle : state_e::Disabled;
    }

    /**
     * @brief Read one trimmed sysfs attribute without invoking external tools.
     *
     * @param path Sysfs attribute path.
     * @return Attribute content, or an empty string when it could not be read.
     */
    std::string read_attribute(const std::filesystem::path &path) {
      std::ifstream input {path};
      std::string value;
      std::getline(input, value);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
      }
      return value;
    }

    /**
     * @brief Remove configuration whitespace from a GPU selector.
     *
     * @param value Raw configuration value.
     * @return Selector without leading or trailing whitespace.
     */
    std::string trim_gpu_selector(std::string_view value) {
      const auto first {std::find_if_not(value.begin(), value.end(), [](const unsigned char character) {
        return std::isspace(character);
      })};
      const auto last {std::find_if_not(
                         value.rbegin(),
                         value.rend(),
                         [](const unsigned char character) {
                           return std::isspace(character);
                         }
      )
                         .base()};
      return first >= last ? std::string {} : std::string {first, last};
    }

    /**
     * @brief Describe an AMD DRM render node from its sysfs device directory.
     */
    struct gpu_candidate_t {
      std::string pci_bdf;  ///< Canonical PCI BDF independent of DRM node numbering.
      std::string card_node;  ///< DRM card node corresponding to the selected render node.
      std::string render_node;  ///< DRM render node path.
      std::string gamescope_device;  ///< PCI vendor/device string accepted by Gamescope.
      std::uint64_t vram_bytes {};  ///< Dedicated VRAM reported by amdgpu.
    };

    /**
     * @brief Record DRM render-node access observations for a selector diagnostic.
     */
    struct render_node_access_t {
      bool exists {false};  ///< Whether the node was visible to the service process.
      bool character_device {false};  ///< Whether the node has DRM character-device type.
      bool readable {false};  ///< Whether the effective user may read the node.
      bool writable {false};  ///< Whether the effective user may write the node.
      bool open_ok {false};  ///< Whether opening the node with read/write access succeeded.
      int open_errno {0};  ///< Error reported by the failed open call, or zero on success.
    };

    /**
     * @brief Inspect a DRM node using the same process credentials as the service.
     *
     * @param render_node Candidate DRM node path.
     * @return Existence, type, permission, and open observations.
     */
    render_node_access_t inspect_render_node(const std::filesystem::path &render_node) {
      render_node_access_t result;
#if defined(__linux__)
      std::error_code error;
      const auto node_status {std::filesystem::status(render_node, error)};
      result.exists = !error && std::filesystem::exists(node_status);
      result.character_device = !error && std::filesystem::is_character_file(node_status);
      result.readable = ::access(render_node.c_str(), R_OK) == 0;
      result.writable = ::access(render_node.c_str(), W_OK) == 0;
      if (!result.character_device) {
        return result;
      }
      const int descriptor {::open(render_node.c_str(), O_RDWR | O_CLOEXEC)};
      if (descriptor < 0) {
        result.open_errno = errno;
        return result;
      }
      result.open_ok = true;
      ::close(descriptor);
#else
      (void) render_node;
#endif
      return result;
    }

    /**
     * @brief Verify that a DRM render node is a usable character device.
     *
     * @param render_node Candidate render node.
     * @return True only when the current service user can open the node read/write.
     */
    bool accessible_render_node(const std::filesystem::path &render_node) {
      return inspect_render_node(render_node).open_ok;
    }

    /**
     * @brief Resolve a DRM render node to an AMD GPU descriptor.
     *
     * @param render_node Candidate `/dev/dri/renderD*` node.
     * @return AMD descriptor, or no value when the node is not an AMD device.
     */
    std::optional<gpu_candidate_t> amd_gpu_from_render_node(const std::filesystem::path &render_node) {
      const auto sys_device {std::filesystem::path {"/sys/class/drm"} / render_node.filename() / "device"};
      const auto vendor {read_attribute(sys_device / "vendor")};
      const auto device {read_attribute(sys_device / "device")};
      std::error_code driver_error;
      const auto driver {std::filesystem::canonical(sys_device / "driver", driver_error).filename().string()};
      if (vendor != "0x1002" || device.size() != 6 || driver_error || driver != "amdgpu" || !accessible_render_node(render_node)) {
        return std::nullopt;
      }
      gpu_candidate_t candidate;
      std::error_code canonical_error;
      candidate.pci_bdf = std::filesystem::canonical(sys_device, canonical_error).filename().string();
      if (canonical_error || candidate.pci_bdf.empty()) {
        return std::nullopt;
      }
      candidate.render_node = render_node.string();
      candidate.gamescope_device = vendor.substr(2) + ":" + device.substr(2);
      std::error_code iterator_error;
      for (const auto &entry : std::filesystem::directory_iterator {"/sys/class/drm", iterator_error}) {
        const auto name {entry.path().filename().string()};
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) {
          continue;
        }
        std::error_code card_error;
        const auto card_device {std::filesystem::canonical(entry.path() / "device", card_error)};
        if (!card_error && card_device.filename() == candidate.pci_bdf) {
          candidate.card_node = (std::filesystem::path {"/dev/dri"} / name).string();
          break;
        }
      }
      try {
        candidate.vram_bytes = std::stoull(read_attribute(sys_device / "mem_info_vram_total"));
      } catch (const std::exception &) {
        candidate.vram_bytes = 0;
      }
      return candidate;
    }

    /**
     * @brief Select an AMD dGPU without ever choosing a small-UMA iGPU by default.
     *
     * @param requested GPU selector configured as a render node or Gamescope PCI identifier.
     * @param error Receives a user-facing selection failure.
     * @return Selected GPU descriptor.
     */
    std::optional<gpu_candidate_t> select_amd_dgpu(const std::string &requested, std::string &error) {
      const auto selector {trim_gpu_selector(requested)};
      if (!selector.empty() && selector.find(':') != std::string::npos && selector.find('/') == std::string::npos && selector.find('.') == std::string::npos) {
        if (selector.rfind("1002:", 0) != 0) {
          error = "SteamOS virtual display requires an AMD GPU identifier";
          return std::nullopt;
        }
#ifdef SUNSHINE_TESTS
        // Unit tests use a synthetic Gamescope PCI identifier because CI has no DRM GPU.
        return gpu_candidate_t {"test-pci-bdf", "", "", selector, 0};
#else
        error = "Configure the SteamOS GPU as a PCI BDF or DRM render node";
        return std::nullopt;
#endif
      }
      if (!selector.empty()) {
        if (selector.find(':') != std::string::npos && selector.find('.') != std::string::npos) {
          std::error_code iterator_error;
          for (const auto &entry : std::filesystem::directory_iterator {"/dev/dri", iterator_error}) {
            const auto name {entry.path().filename().string()};
            if (name.rfind("renderD", 0) != 0) {
              continue;
            }
            const auto sys_device {std::filesystem::path {"/sys/class/drm"} / name / "device"};
            std::error_code canonical_error;
            if (std::filesystem::canonical(sys_device, canonical_error).filename() == selector) {
              if (const auto candidate {amd_gpu_from_render_node(entry.path())}) {
                return candidate;
              }
            }
          }
        }
        const auto explicit_node {std::filesystem::path {selector.rfind("renderD", 0) == 0 || selector.rfind("card", 0) == 0 ? "/dev/dri/" + selector : selector}};
        if (const auto candidate {amd_gpu_from_render_node(explicit_node)}) {
          return candidate;
        }
        if (explicit_node.filename().string().rfind("card", 0) == 0) {
          std::error_code card_error;
          const auto card_device {std::filesystem::canonical(std::filesystem::path {"/sys/class/drm"} / explicit_node.filename() / "device", card_error)};
          if (!card_error) {
            std::error_code iterator_error;
            for (const auto &entry : std::filesystem::directory_iterator {"/dev/dri", iterator_error}) {
              if (entry.path().filename().string().rfind("renderD", 0) != 0) {
                continue;
              }
              std::error_code render_error;
              const auto render_device {std::filesystem::canonical(std::filesystem::path {"/sys/class/drm"} / entry.path().filename() / "device", render_error)};
              if (!render_error && render_device == card_device) {
                if (const auto candidate {amd_gpu_from_render_node(entry.path())}) {
                  return candidate;
                }
              }
            }
          }
        }
        const auto access {inspect_render_node(explicit_node)};
        const auto sys_device {std::filesystem::path {"/sys/class/drm"} / explicit_node.filename() / "device"};
        std::error_code driver_error;
        const auto driver {std::filesystem::canonical(sys_device / "driver", driver_error).filename().string()};
        BOOST_LOG(warning) << "GPU_SELECTOR configured_value=" << selector << " normalized_input=" << explicit_node
                           << " resolved_render_node=" << explicit_node << " resolved_card_node="
                           << (explicit_node.filename().string().rfind("card", 0) == 0 ? explicit_node.string() : "")
                           << " resolved_pci_bdf=" << std::filesystem::canonical(sys_device, driver_error).filename().string()
                           << " driver=" << (driver_error ? "unresolved" : driver)
                           << " vendor_id=" << read_attribute(sys_device / "vendor")
                           << " device_id=" << read_attribute(sys_device / "device")
                           << " exists=" << access.exists << " is_char_device=" << access.character_device
                           << " readable=" << access.readable << " writable=" << access.writable
                           << " open_ok=" << access.open_ok << " open_errno=" << access.open_errno
                           << " current_uid=" << ::getuid()
                           << " failure_reason=not_accessible_amd_render_node";
        error = "Configured SteamOS game GPU is not an accessible AMD DRM render node";
        return std::nullopt;
      }
      std::optional<gpu_candidate_t> selected;
      std::error_code iterator_error;
      for (const auto &entry : std::filesystem::directory_iterator {"/dev/dri", iterator_error}) {
        const auto name {entry.path().filename().string()};
        if (name.rfind("renderD", 0) != 0) {
          continue;
        }
        const auto candidate {amd_gpu_from_render_node(entry.path())};
        if (candidate && (!selected || candidate->vram_bytes > selected->vram_bytes)) {
          selected = candidate;
        }
      }
      constexpr std::uint64_t minimum_discrete_vram {1024ULL * 1024ULL * 1024ULL};
      if (!selected || selected->vram_bytes < minimum_discrete_vram) {
        error = "No discrete AMD GPU with at least 1 GiB of dedicated VRAM was found";
        return std::nullopt;
      }
      return selected;
    }

    /**
     * @brief Verify that Gamescope's vendor/device selector identifies one GPU.
     *
     * Gamescope advertises `--prefer-vk-device` as a vendor/device selector,
     * not a PCI-BDF selector.  A second AMD adapter with the same identifier
     * would make a requested BDF ambiguous, so the virtual stream must fail
     * rather than potentially rendering on a different GPU.
     *
     * @param selected GPU selected from configuration.
     * @param error Receives a user-facing selector failure.
     * @return True when the selector maps to at most one AMD render node.
     */
    bool gamescope_selector_is_unambiguous(const gpu_candidate_t &selected, std::string &error) {
#ifdef SUNSHINE_TESTS
      if (selected.render_node.empty()) {
        return true;
      }
#endif
      std::size_t matching_devices {};
      std::error_code iterator_error;
      for (const auto &entry : std::filesystem::directory_iterator {"/dev/dri", iterator_error}) {
        if (entry.path().filename().string().rfind("renderD", 0) != 0) {
          continue;
        }
        const auto candidate {amd_gpu_from_render_node(entry.path())};
        if (candidate && candidate->gamescope_device == selected.gamescope_device) {
          ++matching_devices;
        }
      }
      if (matching_devices > 1) {
        error = "Gamescope cannot unambiguously select the requested AMD PCI BDF because multiple GPUs share its vendor/device identifier";
        return false;
      }
      return true;
    }

    /**
     * @brief Get the per-user base runtime directory without persistent writes.
     *
     * @return Base path below the user runtime directory.
     */
    std::filesystem::path runtime_base() {
      if (!config::steamos_virtual_display.runtime_directory.empty()) {
        return config::steamos_virtual_display.runtime_directory;
      }
      const auto *runtime {std::getenv("XDG_RUNTIME_DIR")};
      return runtime ? std::filesystem::path {runtime} / "steamshine" : std::filesystem::path {};
    }

    /**
     * @brief Check whether a prospective session path remains in the user runtime directory.
     *
     * Canonicalizing both paths prevents a configured symlink from redirecting
     * volatile session state into a persistent or foreign directory.
     *
     * @param candidate Runtime base configured for SteamShine sessions.
     * @param runtime_root User-owned XDG runtime directory.
     * @return True only when @p candidate is equal to or below @p runtime_root.
     */
    bool path_is_within_runtime_root(const std::filesystem::path &candidate, const std::filesystem::path &runtime_root) {
      std::error_code error;
      const auto canonical_candidate {std::filesystem::weakly_canonical(candidate, error)};
      if (error) {
        return false;
      }
      const auto canonical_root {std::filesystem::weakly_canonical(runtime_root, error)};
      if (error) {
        return false;
      }
      auto candidate_part {canonical_candidate.begin()};
      for (const auto &root_part : canonical_root) {
        if (candidate_part == canonical_candidate.end() || *candidate_part != root_part) {
          return false;
        }
        ++candidate_part;
      }
      return true;
    }

#if defined(__linux__)
    /**
     * @brief Check the installed Gamescope help text before using version-specific options.
     *
     * @param executable Gamescope executable selected by configuration.
     * @return True when the required headless option set is advertised.
     */
    bool read_gamescope_help(const std::string &executable, std::string &help) {
      int pipe_fds[2] {};
      if (::pipe(pipe_fds) != 0) {
        return false;
      }
      const pid_t child {::fork()};
      if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
      }
      if (child == 0) {
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::execlp(executable.c_str(), executable.c_str(), "--help", nullptr);
        _exit(127);
      }
      ::close(pipe_fds[1]);
      const int flags {::fcntl(pipe_fds[0], F_GETFL)};
      if (flags < 0 || ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(pipe_fds[0]);
        ::kill(child, SIGTERM);
        ::waitpid(child, nullptr, 0);
        return false;
      }
      std::array<char, 4096> buffer {};
      int status {};
      bool exited {false};
      const auto deadline {std::chrono::steady_clock::now() + std::chrono::seconds {5}};
      while (std::chrono::steady_clock::now() < deadline && help.size() < 65536) {
        pollfd descriptor {.fd = pipe_fds[0], .events = POLLIN, .revents = 0};
        if (::poll(&descriptor, 1, 100) > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
          while (help.size() < 65536) {
            const auto bytes {::read(pipe_fds[0], buffer.data(), buffer.size())};
            if (bytes > 0) {
              help.append(buffer.data(), static_cast<std::size_t>(bytes));
              continue;
            }
            break;
          }
        }
        exited = ::waitpid(child, &status, WNOHANG) == child;
        if (exited) {
          break;
        }
      }
      ::close(pipe_fds[0]);
      if (!exited) {
        ::kill(child, SIGTERM);
        ::waitpid(child, &status, 0);
      }
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !help.empty();
    }
#endif
  }  // namespace

  bool prepare(const rtsp_stream::launch_session_t &launch_session, std::string &error) {
    std::scoped_lock lock {manager.mutex};
    const auto decision {decide_virtual_display({
      config::steamos_virtual_display.enabled,
      config::steamos_virtual_display.mode,
      false,
      false,
      false,
      false,
      true,
    })};
    if (!decision.required) {
      manager.current = state_e::Disabled;
      return true;
    }
    BOOST_LOG(info) << "SESSION_EVENT mode_decided mode=" << to_string(config::steamos_virtual_display.mode) << " reason=" << decision.reason;
#if !defined(__linux__)
    error = "SteamOS virtual display is only available on Linux";
    manager.current = state_e::Disabled;
    return false;
#else
    if (manager.current == state_e::Failed) {
      manager.current = state_e::Recovering;
      recover_failed_session_locked();
      BOOST_LOG(info) << "SteamOS virtual display recovery completed";
    }
    if (manager.current != state_e::Idle && manager.current != state_e::Disabled) {
      error = "A SteamShine virtual display session is already active";
      return false;
    }
    std::string help_text;
    if (!read_gamescope_help(config::steamos_virtual_display.gamescope_path, help_text)) {
      error = "Failed to read installed Gamescope help";
      manager.current = state_e::Failed;
      return false;
    }
    const auto request {normalize_display_request(launch_session.width, launch_session.height, launch_session.fps, config::steamos_virtual_display.default_width, config::steamos_virtual_display.default_height, config::steamos_virtual_display.default_fps)};
    const auto gpu {select_amd_dgpu(config::steamos_virtual_display.game_gpu, error)};
    if (!gpu) {
      manager.current = state_e::Failed;
      return false;
    }
    if (!gamescope_selector_is_unambiguous(*gpu, error)) {
      manager.current = state_e::Failed;
      return false;
    }
    const auto capture_gpu {select_amd_dgpu(config::steamos_virtual_display.capture_gpu.empty() ? config::steamos_virtual_display.game_gpu : config::steamos_virtual_display.capture_gpu, error)};
    if (!capture_gpu) {
      manager.current = state_e::Failed;
      return false;
    }
    const auto encoder_gpu {select_amd_dgpu(config::steamos_virtual_display.encoder_gpu.empty() ? config::steamos_virtual_display.game_gpu : config::steamos_virtual_display.encoder_gpu, error)};
    if (!encoder_gpu) {
      manager.current = state_e::Failed;
      return false;
    }
    const bool capture_matches {
      (!gpu->render_node.empty() && gpu->render_node == capture_gpu->render_node) ||
      (gpu->render_node.empty() && gpu->gamescope_device == capture_gpu->gamescope_device)
    };
    const bool encoder_matches {
      (!gpu->render_node.empty() && gpu->render_node == encoder_gpu->render_node) ||
      (gpu->render_node.empty() && gpu->gamescope_device == encoder_gpu->gamescope_device)
    };
    if (!capture_matches || !encoder_matches) {
      error = "SteamOS virtual display requires game rendering, capture, and encoding to use one AMD dGPU";
      manager.current = state_e::Failed;
      return false;
    }
    const auto arguments {gamescope_arguments(help_text, request.width, request.height, request.fps, launch_session.enable_hdr, gpu->gamescope_device, error)};
    if (arguments.empty()) {
      manager.current = state_e::Failed;
      return false;
    }
    const auto *runtime_root_value {std::getenv("XDG_RUNTIME_DIR")};
    const auto base {runtime_base()};
    const auto runtime_root {runtime_root_value ? std::filesystem::path {runtime_root_value} : std::filesystem::path {}};
    if (base.empty() || runtime_root.empty() || !std::filesystem::is_directory(runtime_root) || !path_is_within_runtime_root(base, runtime_root)) {
      error = "XDG_RUNTIME_DIR is unavailable; refusing persistent runtime fallback";
      manager.current = state_e::Failed;
      return false;
    }
    manager.runtime_directory = base / ("session-" + std::to_string(::getpid()) + "-" + std::to_string(launch_session.id));
    manager.pci_bdf = gpu->pci_bdf;
    manager.render_node = gpu->render_node;
    manager.packet_tracking.store(false, std::memory_order_release);
    manager.encoded_packets.store(0, std::memory_order_relaxed);
    manager.encoded_bytes.store(0, std::memory_order_relaxed);
    manager.idr_packets.store(0, std::memory_order_relaxed);
    manager.captured_frames.store(0, std::memory_order_relaxed);
    std::error_code ec;
    std::filesystem::create_directories(manager.runtime_directory, ec);
    if (ec) {
      error = "Failed to create owned virtual-session runtime directory";
      manager.current = state_e::Failed;
      return false;
    }
    std::filesystem::permissions(manager.runtime_directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    if (ec) {
      std::filesystem::remove_all(manager.runtime_directory, ec);
      error = "Failed to restrict owned virtual-session runtime directory";
      manager.current = state_e::Failed;
      return false;
    }
    const auto socket {manager.runtime_directory / "gamescope-0"};
    {
      std::ofstream marker {manager.runtime_directory / owner_marker_name.data(), std::ios::binary | std::ios::trunc};
      marker << owner_marker_contents;
      if (!marker) {
        std::filesystem::remove_all(manager.runtime_directory, ec);
        error = "Failed to mark owned virtual-session runtime directory";
        manager.current = state_e::Failed;
        return false;
      }
    }
    manager.current = state_e::Starting;
    const pid_t child {::fork()};
    if (child == 0) {
      ::setpgid(0, 0);
      const auto path {config::steamos_virtual_display.gamescope_path};
      const auto runtime {manager.runtime_directory.string()};
      ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
      // A headless Gamescope owns its Wayland server.  Inheriting the desktop
      // display name makes Gamescope try to connect to a non-existent parent
      // socket below this private runtime directory before it starts that
      // server.
      ::unsetenv("WAYLAND_DISPLAY");
      ::unsetenv("DISPLAY");
      std::vector<char *> argv;
      argv.reserve(arguments.size() + 5);
      argv.push_back(const_cast<char *>(path.c_str()));
      for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
      }
      argv.push_back(const_cast<char *>("--"));
      argv.push_back(const_cast<char *>("/bin/sh"));
      argv.push_back(const_cast<char *>("-c"));
      argv.push_back(const_cast<char *>("exec sleep infinity"));
      argv.push_back(nullptr);
      ::execvp(path.c_str(), argv.data());
      _exit(127);
    }
    if (child < 0) {
      std::filesystem::remove_all(manager.runtime_directory, ec);
      error = "Failed to fork Gamescope";
      manager.current = state_e::Failed;
      return false;
    }
    if (::setpgid(child, child) != 0 && errno != EACCES) {
      ::kill(child, SIGTERM);
      ::waitpid(child, nullptr, 0);
      std::filesystem::remove_all(manager.runtime_directory, ec);
      error = "Failed to create an owned Gamescope process group";
      manager.current = state_e::Failed;
      return false;
    }
    manager.process_group = child;
    {
      std::ofstream pid_file {manager.runtime_directory / gamescope_pid_name.data(), std::ios::trunc};
      pid_file << child << '\n';
      if (!pid_file) {
        stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
        manager.process_group = -1;
        std::filesystem::remove_all(manager.runtime_directory, ec);
        error = "Failed to record owned Gamescope process identity";
        manager.current = state_e::Failed;
        return false;
      }
    }
    manager.current = state_e::WaitingForDisplay;
    const auto deadline {std::chrono::steady_clock::now() + std::chrono::seconds {config::steamos_virtual_display.startup_timeout_seconds}};
    while (std::chrono::steady_clock::now() < deadline) {
      int status {};
      if (::waitpid(child, &status, WNOHANG) == child) {
        error = "Gamescope exited before its Wayland socket became ready";
        manager.current = state_e::Failed;
        break;
      }
      if (owned_wayland_socket_exists(socket)) {
        manager.current = state_e::WaitingForCapture;
        BOOST_LOG(info) << "SteamOS virtual display socket ready: " << request.width << 'x' << request.height << '@' << request.fps << " on AMD PCI " << manager.pci_bdf << " (" << manager.render_node << ')';
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (error.empty()) {
      error = "Timed out waiting for the Gamescope Wayland socket";
      manager.current = state_e::Failed;
    }
    stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
    std::filesystem::remove_all(manager.runtime_directory, ec);
    manager.process_group = -1;
    return false;
#endif
  }

  bool capture_backend_required() {
    return decide_virtual_display({
                                    config::steamos_virtual_display.enabled,
                                    config::steamos_virtual_display.mode,
                                    false,
                                    false,
                                    false,
                                    false,
                                    true,
                                  })
      .required;
  }

  std::string_view to_string(const state_e state) {
    switch (state) {
      case state_e::Disabled:
        return "Disabled";
      case state_e::Idle:
        return "Idle";
      case state_e::Starting:
        return "Starting";
      case state_e::WaitingForDisplay:
        return "WaitingForDisplay";
      case state_e::WaitingForCapture:
        return "WaitingForCapture";
      case state_e::Ready:
        return "Ready";
      case state_e::Streaming:
        return "Streaming";
      case state_e::Stopping:
        return "Stopping";
      case state_e::Failed:
        return "Failed";
      case state_e::Recovering:
        return "Recovering";
    }
    return "Disabled";
  }

  status_snapshot_t status_snapshot() {
    std::scoped_lock lock {manager.mutex};
    status_snapshot_t snapshot;
    snapshot.state = manager.current;
    snapshot.runtime_directory = manager.runtime_directory.string();
    snapshot.pci_bdf = manager.pci_bdf;
    snapshot.render_node = manager.render_node;
    const auto socket {manager.runtime_directory / "gamescope-0"};
    if (!manager.runtime_directory.empty() && owned_wayland_socket_exists(socket)) {
      snapshot.socket_path = socket.string();
    }
#if defined(__linux__)
    snapshot.gamescope_pid = manager.process_group;
#endif
    snapshot.captured_frames = manager.captured_frames.load(std::memory_order_relaxed);
    snapshot.encoded_packets = manager.encoded_packets.load(std::memory_order_relaxed);
    snapshot.encoded_bytes = manager.encoded_bytes.load(std::memory_order_relaxed);
    snapshot.idr_packets = manager.idr_packets.load(std::memory_order_relaxed);
    return snapshot;
  }

  void cleanup_orphan_sessions() {
#if defined(__linux__)
    if (!config::steamos_virtual_display.enabled || !config::steamos_virtual_display.cleanup_orphan_sessions) {
      return;
    }
    const auto base {runtime_base()};
    const auto *runtime_root_value {std::getenv("XDG_RUNTIME_DIR")};
    const auto runtime_root {runtime_root_value ? std::filesystem::path {runtime_root_value} : std::filesystem::path {}};
    if (base.empty() || runtime_root.empty() || !std::filesystem::is_directory(runtime_root) || !path_is_within_runtime_root(base, runtime_root)) {
      BOOST_LOG(warning) << "SteamOS virtual display refused orphan cleanup outside XDG_RUNTIME_DIR";
      return;
    }
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator {base, error}) {
      if (error || !entry.is_directory(error) || entry.is_symlink(error) || !entry.path().filename().string().starts_with("session-")) {
        continue;
      }
      std::ifstream marker {entry.path() / owner_marker_name.data(), std::ios::binary};
      const std::string contents {std::istreambuf_iterator<char> {marker}, {}};
      if (contents != owner_marker_contents) {
        continue;
      }
      BOOST_LOG(warning) << "Cleaning orphaned SteamOS virtual session runtime: " << entry.path();
      stop_processes_using_runtime_directory(entry.path(), std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
      std::filesystem::remove_all(entry.path(), error);
    }
#endif
  }

  void mark_streaming() {
    std::scoped_lock lock {manager.mutex};
    manager.stream_requested = true;
    if (manager.current == state_e::Ready) {
      manager.current = state_e::Streaming;
      BOOST_LOG(info) << "SteamOS virtual display streaming started";
    }
    manager.packet_tracking.store(true, std::memory_order_release);
  }

  void mark_encoded_packet(size_t bytes, bool idr) {
    if (!manager.packet_tracking.load(std::memory_order_acquire)) {
      return;
    }
    manager.encoded_packets.fetch_add(1, std::memory_order_relaxed);
    manager.encoded_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (idr) {
      manager.idr_packets.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void mark_captured_frame() {
    if (manager.packet_tracking.load(std::memory_order_acquire)) {
      manager.captured_frames.fetch_add(1, std::memory_order_relaxed);
    }
  }

  bool application_environment(std::string &runtime_directory, std::string &wayland_display) {
    std::scoped_lock lock {manager.mutex};
    if (manager.runtime_directory.empty() || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      return false;
    }
    runtime_directory = manager.runtime_directory.string();
    wayland_display = "gamescope-0";
    return true;
  }

  bool capture_socket(std::string &socket_path) {
    std::scoped_lock lock {manager.mutex};
    const auto socket {manager.runtime_directory / "gamescope-0"};
    if (manager.runtime_directory.empty() || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming) || !owned_wayland_socket_exists(socket)) {
      return false;
    }
    socket_path = socket.string();
    return true;
  }

  bool capture_socket_required() {
    std::scoped_lock lock {manager.mutex};
    return !manager.runtime_directory.empty() &&
           (manager.current == state_e::WaitingForCapture || manager.current == state_e::Ready || manager.current == state_e::Streaming || manager.current == state_e::Failed);
  }

  bool encoder_render_node(std::string &render_node) {
    std::scoped_lock lock {manager.mutex};
    if (manager.render_node.empty() || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      return false;
    }
    render_node = manager.render_node;
    return true;
  }

  bool active() {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    return manager.process_group > 0 && !manager.runtime_directory.empty() &&
           (manager.current == state_e::WaitingForCapture || manager.current == state_e::Ready || manager.current == state_e::Streaming || manager.current == state_e::Failed);
#else
    return false;
#endif
  }

  void mark_capture_ready() {
    std::scoped_lock lock {manager.mutex};
    if (manager.current == state_e::WaitingForCapture) {
      manager.current = manager.stream_requested ? state_e::Streaming : state_e::Ready;
      BOOST_LOG(info) << "SteamOS virtual display capture attached";
    }
  }

  void mark_capture_lost() {
    std::scoped_lock lock {manager.mutex};
    if (manager.process_group > 0 && (manager.current == state_e::WaitingForCapture || manager.current == state_e::Ready || manager.current == state_e::Streaming)) {
      manager.packet_tracking.store(false, std::memory_order_release);
      manager.current = state_e::Failed;
      BOOST_LOG(error) << "SteamOS virtual display capture source disappeared";
    }
  }

  void stop() {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    if (manager.process_group > 0) {
      manager.current = state_e::Stopping;
      manager.packet_tracking.store(false, std::memory_order_release);
      BOOST_LOG(info) << "SteamOS virtual display stopping owned Gamescope session";
      BOOST_LOG(info) << "SteamOS virtual display encoded packets=" << manager.encoded_packets.load(std::memory_order_relaxed)
                      << " bytes=" << manager.encoded_bytes.load(std::memory_order_relaxed)
                      << " idr=" << manager.idr_packets.load(std::memory_order_relaxed)
                      << " captured_frames=" << manager.captured_frames.load(std::memory_order_relaxed);
      stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
      manager.process_group = -1;
    }
#endif
    manager.current = state_e::Recovering;
    recover_failed_session_locked();
  }

  state_e state() {
    std::scoped_lock lock {manager.mutex};
    return manager.current;
  }
}  // namespace steamos_virtual_session
