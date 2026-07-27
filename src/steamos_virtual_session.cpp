/**
 * @file src/steamos_virtual_session.cpp
 * @brief SteamOS headless Gamescope session lifecycle implementation.
 */
#include "steamos_virtual_session.h"

#include "config.h"
#include "logging.h"
#include "platform/linux/gamescope_source.h"
#include "platform/linux/host_desktop_endpoint.h"
#include "platform/linux/steam_session.h"
#include "rtsp.h"
#include "utility.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/un.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace steamos_virtual_session {
  namespace {
    std::atomic_bool physical_compositor_capture_available {false};  ///< Latest host compositor capture observation.

    struct manager_t {
      std::mutex mutex;  ///< Serializes virtual-session state transitions.
      state_e current {state_e::Disabled};  ///< Current lifecycle state.
      std::filesystem::path runtime_directory;  ///< Runtime path uniquely owned by this process.
      session_origin_e origin {session_origin_e::none};  ///< Origin of the selected Gamescope session.
      bool process_owned {false};  ///< Whether SteamShine may stop the selected process group.
      bool runtime_owned {false};  ///< Whether SteamShine may remove the selected runtime directory.
      std::string source_description;  ///< Human-readable verified source description.
      std::string source_executable;  ///< Verified Gamescope executable path.
      uint64_t source_process_start_time {0};  ///< Verified Gamescope process start time.
      std::string steam_location {"unknown"};  ///< Steam singleton location relative to the selected Gamescope.
      bool migration_required {false};  ///< Whether Desktop Steam needs an explicitly confirmed migration.
      std::string app_launch_rejected_reason;  ///< Stable reason for the latest application launch rejection.
      std::string app_launch_rejected_message;  ///< Safe detail for the latest application launch rejection.
      session_display_endpoint_t display_endpoint;  ///< Verified endpoint for application launches.
      std::string selection_reason;  ///< Stable reason for the current Desktop or Gamescope capture choice.
      presentation_e presentation {presentation_e::remote_only};  ///< Desired remote/local presentation paths.
      bool local_presenter_active {false};  ///< Whether a local presenter has attached successfully.
      std::atomic_uint64_t local_presented_frames {0};  ///< Frames displayed locally.
      std::atomic_uint64_t local_dropped_frames {0};  ///< Latest-frame-wins drops locally.
      std::string pipewire_runtime;  ///< Host PipeWire runtime retained for owned children.
      std::string pipewire_remote;  ///< Host PipeWire remote retained for owned children.
      std::optional<uint32_t> pipewire_node_id;  ///< Verified current-core Gamescope PipeWire node ID.
      std::optional<uint64_t> pipewire_object_serial;  ///< Verified stable Gamescope PipeWire object serial.
      int pipewire_producer_pid {-1};  ///< Verified Gamescope process producing the PipeWire node.
      std::string pulse_runtime;  ///< Host PulseAudio compatibility runtime retained for applications.
      int width {0};  ///< Requested virtual-display width in pixels.
      int height {0};  ///< Requested virtual-display height in pixels.
      int fps {0};  ///< Requested virtual-display refresh rate.
      bool hdr {false};  ///< Whether the retained owned display was created for HDR.
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
      std::filesystem::path verified_input_socket;  ///< EIS socket whose kernel peer matched the selected Gamescope.
      std::uint64_t verified_input_socket_device {0};  ///< Device identity cached after peer verification.
      std::uint64_t verified_input_socket_inode {0};  ///< Inode identity cached after peer verification.
      pid_t verified_input_producer_pid {-1};  ///< Gamescope peer PID cached with the EIS socket.
      std::uint64_t verified_input_producer_start_time {0};  ///< Gamescope start time cached with the EIS socket.
#endif
    } manager;

    constexpr std::string_view owner_marker_name {"steamshine-owner"};
    constexpr std::string_view owner_marker_contents {"steamshine-steamos-virtual-session-v1\n"};
    constexpr std::string_view gamescope_pid_name {"gamescope.pid"};
    constexpr std::string_view display_endpoint_report_name {"display-endpoint.json"};
    std::atomic_uint64_t next_display_generation {1};  ///< Monotonic endpoint generation allocator.

    /**
     * @brief Resolve the login user's PipeWire runtime independently of private Wayland state.
     *
     * @param runtime_root Original login XDG runtime directory.
     * @return Configured host PipeWire runtime, or the original runtime when unset.
     */
    std::filesystem::path host_pipewire_runtime(const std::filesystem::path &runtime_root) {
      return config::steamos_virtual_display.pipewire_runtime.empty() ? runtime_root : std::filesystem::path {config::steamos_virtual_display.pipewire_runtime};
    }

    /**
     * @brief Resolve the host PipeWire remote without accepting a path.
     *
     * @return Configured, inherited, or default PipeWire remote name.
     */
    std::string host_pipewire_remote() {
      if (!config::steamos_virtual_display.pipewire_remote.empty()) {
        return config::steamos_virtual_display.pipewire_remote;
      }
      if (const auto *remote {std::getenv("PIPEWIRE_REMOTE")}; remote && *remote) {
        return remote;
      }
      return "pipewire-0";
    }

    /**
     * @brief Check that a PipeWire remote is a socket name relative to its runtime.
     *
     * @param remote PipeWire remote to validate.
     * @return True when @p remote is a non-empty single path component.
     */
    bool is_valid_pipewire_remote(const std::string_view remote) {
      return !remote.empty() && remote != "." && remote != ".." && remote.find('/') == std::string_view::npos && remote.find('\0') == std::string_view::npos;
    }

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
     * @brief Read the server PID authenticated by a connected UNIX-domain socket.
     *
     * Linux supplies `SO_PEERCRED` from the kernel, so set-user-ID/capability
     * process protections on `/proc/<pid>/fd` do not prevent identity checks.
     *
     * @param path Listening UNIX-domain socket to probe.
     * @return Kernel-authenticated peer PID, or no value when connection fails.
     */
    std::optional<pid_t> unix_socket_peer_pid(const std::filesystem::path &path) {
      const auto native_path {path.string()};
      sockaddr_un address {};
      if (native_path.empty() || native_path.size() >= sizeof(address.sun_path)) {
        return std::nullopt;
      }
      const int descriptor {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      if (descriptor < 0) {
        return std::nullopt;
      }
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, native_path.c_str(), native_path.size() + 1);
      if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return std::nullopt;
      }
      ucred credentials {};
      socklen_t credentials_size {sizeof(credentials)};
      const bool verified {
        ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) == 0 &&
        credentials_size == sizeof(credentials) && credentials.pid > 0
      };
      ::close(descriptor);
      return verified ? std::optional<pid_t> {credentials.pid} : std::nullopt;
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
      if (manager.process_owned && manager.process_group > 0) {
        stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
      }
      manager.process_group = -1;
      manager.verified_input_socket.clear();
      manager.verified_input_socket_device = 0;
      manager.verified_input_socket_inode = 0;
      manager.verified_input_producer_pid = -1;
      manager.verified_input_producer_start_time = 0;
#endif
      std::error_code error;
      if (manager.runtime_owned) {
        std::filesystem::remove_all(manager.runtime_directory, error);
      }
      manager.runtime_directory.clear();
      manager.origin = session_origin_e::none;
      manager.process_owned = false;
      manager.runtime_owned = false;
      manager.source_description.clear();
      manager.source_executable.clear();
      manager.source_process_start_time = 0;
      manager.display_endpoint = {};
      if (manager.app_launch_rejected_reason.empty()) {
        manager.steam_location = "unknown";
      }
      manager.presentation = presentation_e::remote_only;
      manager.local_presenter_active = false;
      manager.local_presented_frames.store(0, std::memory_order_relaxed);
      manager.local_dropped_frames.store(0, std::memory_order_relaxed);
      manager.pipewire_runtime.clear();
      manager.pipewire_remote.clear();
      manager.pipewire_node_id.reset();
      manager.pipewire_object_serial.reset();
      manager.pipewire_producer_pid = -1;
      manager.pulse_runtime.clear();
      manager.width = 0;
      manager.height = 0;
      manager.fps = 0;
      manager.hdr = false;
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
     * @brief Check whether a physical DRM connector is currently connected.
     *
     * @return True when a non-writeback DRM connector reports `connected`.
     */
    bool host_physical_output_connected() {
#if defined(__linux__)
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator {"/sys/class/drm", error}) {
        const auto name {entry.path().filename().string()};
        if (error || name.find('-') == std::string::npos || name.find("Writeback") != std::string::npos) {
          continue;
        }
        std::ifstream status {entry.path() / "status"};
        std::string value;
        std::getline(status, value);
        if (value == "connected") {
          return true;
        }
      }
#endif
      return false;
    }

    /**
     * @brief Check whether a connected DRM output has an enabled scanout.
     *
     * A connector can remain physically connected while its CRTC is disabled.
     * Treating that state as a normal Desktop capture target would make auto
     * mode skip the virtual display even though no desktop frame can advance.
     *
     * @return True when one non-writeback connector reports `enabled`.
     */
    bool host_active_crtc_present() {
#if defined(__linux__)
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator {"/sys/class/drm", error}) {
        const auto name {entry.path().filename().string()};
        if (error || name.find('-') == std::string::npos || name.find("Writeback") != std::string::npos) {
          continue;
        }
        std::ifstream enabled {entry.path() / "enabled"};
        std::string value;
        std::getline(enabled, value);
        if (value == "enabled") {
          return true;
        }
      }
#endif
      return false;
    }

    /**
     * @brief Probe whether automatic policy must prefer a resident Game Mode source.
     *
     * This lightweight launch-time probe runs before the Desktop/virtual
     * decision. It prevents an active Game Mode session from being mistaken
     * for a normal physical Desktop merely because its connector is enabled.
     *
     * @return True when at least one current-user Game Mode source is verified.
     */
    bool verified_game_mode_source_present() {
#if defined(__linux__)
      if (config::steamos_virtual_display.session_source == session_source_policy_e::owned_private) {
        return false;
      }
      const auto *const inherited_runtime {std::getenv("XDG_RUNTIME_DIR")};
      const auto *const inherited_remote {std::getenv("PIPEWIRE_REMOTE")};
      const std::string runtime {
        config::steamos_virtual_display.pipewire_runtime.empty() ? (inherited_runtime ? inherited_runtime : "") : config::steamos_virtual_display.pipewire_runtime
      };
      const std::string remote {
        config::steamos_virtual_display.pipewire_remote.empty() ? (inherited_remote && *inherited_remote ? inherited_remote : "pipewire-0") : config::steamos_virtual_display.pipewire_remote
      };
      if (runtime.empty() || remote.empty()) {
        return false;
      }
      std::string discovery_error;
      const auto timeout {std::min(
        std::chrono::milliseconds {250},
        std::chrono::milliseconds {config::steamos_virtual_display.pipewire_node_timeout_milliseconds}
      )};
      const auto sources {gamescope_source::discover_gamescope_sources(runtime, remote, timeout, discovery_error)};
      return std::ranges::any_of(sources, [](const gamescope_source::gamescope_source_t &source) {
        return source.identity_verified && source.game_mode_verified &&
               source.origin == session_origin_e::attached_existing &&
               gamescope_source::source_identity_is_current(source);
      });
#else
      return false;
#endif
    }

    /**
     * @brief Decide whether the selected session may use a local mirror.
     *
     * @param origin Ownership origin selected for the session.
     * @return Safe desired presentation paths.
     */
    presentation_e desired_presentation(const session_origin_e origin) {
      host_desktop_endpoint::capture();
      const auto endpoint {host_desktop_endpoint::current()};
      return decide_presentation(config::steamos_virtual_display.local_presentation, origin, !endpoint.xdg_runtime_directory.empty() && !endpoint.wayland_display.empty(), host_physical_output_connected());
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

    /**
     * @brief Resolve a local X11 display name to its UNIX socket.
     *
     * @param display Display spelling such as `:1`, `:2.0`, or `:27`.
     * @return X11 socket path, or no value for remote or malformed displays.
     */
    std::optional<std::filesystem::path> x11_socket_path(const std::string_view display) {
      if (display.size() < 2 || display.front() != ':') {
        return std::nullopt;
      }
      const auto screen_separator {display.find('.', 1)};
      const auto number {display.substr(1, screen_separator == std::string_view::npos ? std::string_view::npos : screen_separator - 1)};
      if (number.empty() || !std::ranges::all_of(number, [](const unsigned char character) {
            return std::isdigit(character);
          })) {
        return std::nullopt;
      }
      return std::filesystem::path {"/tmp/.X11-unix"} / ("X" + std::string {number});
    }

    /**
     * @brief Validate a current-user regular file without following a final symlink.
     *
     * @param path Candidate file path.
     * @return True only for a current-user regular non-symlink file.
     */
    bool current_user_regular_file(const std::filesystem::path &path) {
      struct stat file_stat {};
      return !path.empty() && path.is_absolute() && ::lstat(path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode) && file_stat.st_uid == ::getuid() && (file_stat.st_mode & 0077) == 0;
    }

    /**
     * @brief Validate a current-user UNIX socket without accepting a symlink.
     *
     * @param path Candidate socket path.
     * @return True only for a current-user UNIX-domain socket.
     */
    bool current_user_socket(const std::filesystem::path &path) {
      struct stat socket_stat {};
      return ::lstat(path.c_str(), &socket_stat) == 0 && S_ISSOCK(socket_stat.st_mode) && socket_stat.st_uid == ::getuid();
    }

    /**
     * @brief Verify that a current-user UNIX socket accepts a local connection.
     *
     * A filesystem socket can remain after its server exits. Connecting with a
     * short non-blocking deadline prevents a stale Xwayland path from becoming
     * a verified application endpoint.
     *
     * @param path Candidate UNIX-domain socket path.
     * @return True only when the owned socket accepts a connection.
     */
    bool current_user_connectable_socket(const std::filesystem::path &path) {
      const auto value {path.string()};
      if (!current_user_socket(path) || value.empty() || value.size() >= sizeof(sockaddr_un::sun_path)) {
        return false;
      }
      const int connection {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
      if (connection < 0) {
        return false;
      }
      const auto close_connection {util::fail_guard([connection]() {
        ::close(connection);
      })};
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, value.c_str(), value.size() + 1);
      if (::connect(connection, reinterpret_cast<const sockaddr *>(&address), offsetof(sockaddr_un, sun_path) + value.size() + 1) == 0) {
        return true;
      }
      if (errno != EINPROGRESS) {
        return false;
      }
      pollfd descriptor {
        .fd = connection,
        .events = POLLOUT,
      };
      if (::poll(&descriptor, 1, 100) != 1) {
        return false;
      }
      int socket_error {};
      socklen_t error_size {sizeof(socket_error)};
      return ::getsockopt(connection, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 && socket_error == 0;
    }

    /**
     * @brief Create the owner-only Xauthority path inherited by owned Gamescope.
     *
     * Gamescope 3.16 starts its private Xwayland without an `-auth` argument,
     * but X11 clients still honor `XAUTHORITY`. Supplying an empty private file
     * preserves Gamescope's access policy while giving every later application
     * the same explicit, validated endpoint path.
     *
     * @param runtime_directory Newly-created owner-only session directory.
     * @param error Receives a stable operator-facing failure description.
     * @return Xauthority path, or no value when it could not be created safely.
     */
    std::optional<std::filesystem::path> create_owned_xauthority(const std::filesystem::path &runtime_directory, std::string &error) {
      const auto path {runtime_directory / "xauthority"};
      const int authority {::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
      if (authority < 0) {
        error = "Failed to create the owned Xwayland authority file";
        return std::nullopt;
      }
      struct stat authority_stat {};
      const bool valid {::fstat(authority, &authority_stat) == 0 && S_ISREG(authority_stat.st_mode) && authority_stat.st_uid == ::getuid() && (authority_stat.st_mode & 0777) == 0600};
      ::close(authority);
      if (!valid) {
        ::unlink(path.c_str());
        error = "Failed to restrict the owned Xwayland authority file";
        return std::nullopt;
      }
      return path;
    }

    /**
     * @brief Retain a local session bus address only when its socket is user-owned.
     *
     * @param address Candidate `DBUS_SESSION_BUS_ADDRESS` value.
     * @return Original address when verified, otherwise an empty string.
     */
    std::string verified_dbus_session_bus_address(const std::string_view address) {
      constexpr std::string_view prefix {"unix:path="};
      if (!address.starts_with(prefix)) {
        return {};
      }
      const auto path {address.substr(prefix.size())};
      if (path.empty() || path.find_first_of(",;") != std::string_view::npos || !current_user_socket(std::filesystem::path {path})) {
        return {};
      }
      return std::string {address};
    }

    /**
     * @brief Read and verify an owned Gamescope bootstrap endpoint report.
     *
     * @param runtime_directory Owned private runtime directory.
     * @param gamescope_pid Expected Gamescope producer PID.
     * @param gamescope_start_time Expected producer start time.
     * @param generation Expected session generation.
     * @param pipewire_runtime Verified host PipeWire runtime.
     * @param pipewire_remote Verified host PipeWire remote.
     * @param pulse_runtime Host PulseAudio compatibility runtime.
     * @return Verified endpoint or a rejected snapshot with a stable error.
     */
    session_display_endpoint_t read_owned_display_endpoint(
      const std::filesystem::path &runtime_directory,
      const int gamescope_pid,
      const uint64_t gamescope_start_time,
      const uint64_t generation,
      const std::string &pipewire_runtime,
      const std::string &pipewire_remote,
      const std::string &pulse_runtime
    ) {
      session_display_endpoint_t endpoint {
        .origin = session_origin_e::owned_private,
        .producer_pid = gamescope_pid,
        .producer_start_time = gamescope_start_time,
        .generation = generation,
        .verification = display_verification_e::rejected,
      };
      const auto report_path {runtime_directory / display_endpoint_report_name};
      struct stat report_stat {};
      if (::lstat(report_path.c_str(), &report_stat) != 0) {
        endpoint.error = "xwayland_display_missing";
        return endpoint;
      }
      if (!S_ISREG(report_stat.st_mode) || report_stat.st_uid != ::getuid() || (report_stat.st_mode & 0777) != 0600) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      try {
        std::ifstream input {report_path};
        boost::property_tree::ptree report;
        boost::property_tree::read_json(input, report);
        if (report.get("owner", std::string {}) != "steamshine" || report.get("generation", uint64_t {}) != generation || report.get("xdg_runtime_directory", std::string {}) != runtime_directory.string()) {
          endpoint.error = "xwayland_identity_unverified";
          return endpoint;
        }
        const int bootstrap_pid {report.get("bootstrap_pid", -1)};
        const uint64_t bootstrap_start_time {report.get("bootstrap_start_time", uint64_t {})};
        const auto bootstrap_identity {gamescope_source::read_process_identity(bootstrap_pid)};
        if (!bootstrap_identity || bootstrap_identity->uid != static_cast<int>(::getuid()) || bootstrap_identity->start_time != bootstrap_start_time || ::getpgid(bootstrap_pid) != gamescope_pid) {
          endpoint.error = "xwayland_identity_unverified";
          return endpoint;
        }
        endpoint.environment_source_pid = bootstrap_pid;
        endpoint.environment_source_start_time = bootstrap_start_time;
        const auto current_identity {gamescope_source::read_process_identity(gamescope_pid)};
        if (!current_identity || current_identity->uid != static_cast<int>(::getuid()) || current_identity->start_time != gamescope_start_time) {
          endpoint.error = "xwayland_identity_unverified";
          return endpoint;
        }
        endpoint.xdg_runtime_directory = runtime_directory.string();
        endpoint.wayland_display = report.get("wayland_display", std::string {});
        endpoint.gamescope_wayland_display = report.get("gamescope_wayland_display", std::string {});
        endpoint.x11_display = report.get("display", std::string {});
        endpoint.xauthority = report.get("xauthority", std::string {});
        endpoint.dbus_session_bus_address = verified_dbus_session_bus_address(report.get("dbus_session_bus_address", std::string {}));
        endpoint.pipewire_runtime_directory = pipewire_runtime;
        endpoint.pipewire_remote = pipewire_remote;
        endpoint.pulse_runtime_path = pulse_runtime;
        const auto x11_socket {x11_socket_path(endpoint.x11_display)};
        if (!x11_socket) {
          endpoint.error = "xwayland_display_missing";
          return endpoint;
        }
        if (!current_user_connectable_socket(*x11_socket)) {
          endpoint.error = "xwayland_identity_unverified";
          return endpoint;
        }
        if (!current_user_regular_file(endpoint.xauthority)) {
          endpoint.error = endpoint.xauthority.empty() ? "xwayland_auth_missing" : "xwayland_identity_unverified";
          return endpoint;
        }
        if (!path_is_within_runtime_root(endpoint.xauthority, runtime_directory)) {
          endpoint.error = "xwayland_identity_unverified";
          return endpoint;
        }
        if (endpoint.wayland_display.empty()) {
          endpoint.wayland_display = "gamescope-0";
        }
        if (endpoint.gamescope_wayland_display.empty()) {
          endpoint.gamescope_wayland_display = endpoint.wayland_display;
        }
        const auto wayland_socket {runtime_directory / endpoint.wayland_display};
        const auto gamescope_wayland_socket {runtime_directory / endpoint.gamescope_wayland_display};
        if (endpoint.wayland_display.find('/') != std::string::npos || endpoint.gamescope_wayland_display.find('/') != std::string::npos || !path_is_within_runtime_root(wayland_socket, runtime_directory) || !path_is_within_runtime_root(gamescope_wayland_socket, runtime_directory) || !current_user_socket(wayland_socket) || !current_user_socket(gamescope_wayland_socket)) {
          endpoint.error = "xwayland_identity_unverified";
          return endpoint;
        }
        endpoint.verification = display_verification_e::verified;
        endpoint.error.clear();
      } catch (const std::exception &) {
        endpoint.error = "xwayland_identity_unverified";
      }
      return endpoint;
    }

    /**
     * @brief Validate a display endpoint read from a resident Steam process.
     *
     * @param environment Allow-listed resident Steam environment.
     * @param gamescope_pid Verified parent Gamescope PID.
     * @param gamescope_start_time Verified parent Gamescope start time.
     * @param generation New endpoint generation.
     * @param pipewire_runtime Verified host PipeWire runtime.
     * @param pipewire_remote Verified host PipeWire remote.
     * @return Verified endpoint or a rejected snapshot with a stable reason.
     */
    session_display_endpoint_t attached_display_endpoint(
      const steam_session::resident_environment_t &environment,
      const int gamescope_pid,
      const uint64_t gamescope_start_time,
      const uint64_t generation,
      const std::string &pipewire_runtime,
      const std::string &pipewire_remote
    ) {
      session_display_endpoint_t endpoint {
        .origin = session_origin_e::attached_existing,
        .xdg_runtime_directory = environment.xdg_runtime_directory,
        .wayland_display = environment.wayland_display,
        .gamescope_wayland_display = environment.gamescope_wayland_display,
        .x11_display = environment.x11_display,
        .xauthority = environment.xauthority,
        .pipewire_runtime_directory = pipewire_runtime,
        .pipewire_remote = pipewire_remote,
        .pulse_runtime_path = (std::filesystem::path {pipewire_runtime} / "pulse").string(),
        .dbus_session_bus_address = verified_dbus_session_bus_address(environment.dbus_session_bus_address),
        .producer_pid = gamescope_pid,
        .producer_start_time = gamescope_start_time,
        .environment_source_pid = environment.steam_pid,
        .environment_source_start_time = environment.steam_start_time,
        .generation = generation,
        .verification = display_verification_e::rejected,
      };
      const auto producer {gamescope_source::read_process_identity(gamescope_pid)};
      if (!producer || producer->uid != static_cast<int>(::getuid()) || producer->start_time != gamescope_start_time) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      const auto x11_socket {x11_socket_path(endpoint.x11_display)};
      if (!x11_socket) {
        endpoint.error = "xwayland_display_missing";
        return endpoint;
      }
      if (!current_user_connectable_socket(*x11_socket)) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      if (!current_user_regular_file(endpoint.xauthority)) {
        endpoint.error = endpoint.xauthority.empty() ? "xwayland_auth_missing" : "xwayland_identity_unverified";
        return endpoint;
      }
      struct stat runtime_stat {};
      const std::filesystem::path runtime {endpoint.xdg_runtime_directory};
      if (!runtime.is_absolute() || ::lstat(runtime.c_str(), &runtime_stat) != 0 || !S_ISDIR(runtime_stat.st_mode) || runtime_stat.st_uid != ::getuid()) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      if (!path_is_within_runtime_root(endpoint.xauthority, runtime)) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      if (!endpoint.wayland_display.empty() && (endpoint.wayland_display.find('/') != std::string::npos || !current_user_socket(runtime / endpoint.wayland_display))) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      if (endpoint.gamescope_wayland_display.empty()) {
        endpoint.gamescope_wayland_display = endpoint.wayland_display;
      }
      if (endpoint.gamescope_wayland_display.find('/') != std::string::npos || !current_user_socket(runtime / endpoint.gamescope_wayland_display)) {
        endpoint.error = "xwayland_identity_unverified";
        return endpoint;
      }
      endpoint.verification = display_verification_e::verified;
      return endpoint;
    }

    /**
     * @brief Validate a host PipeWire runtime without confusing it with private Wayland state.
     *
     * @param runtime Candidate host user runtime directory.
     * @param private_base SteamShine private runtime base which must not host PipeWire.
     * @param error Receives a concise launch failure reason.
     * @return Canonical host runtime when ownership and location checks succeed.
     */
    std::optional<std::filesystem::path> validate_host_pipewire_runtime(const std::filesystem::path &runtime, const std::filesystem::path &private_base, std::string &error) {
      if (runtime.empty()) {
        error = "Host PipeWire runtime is not configured";
        return std::nullopt;
      }
      struct stat configured_runtime_stat {};
      if (::lstat(runtime.c_str(), &configured_runtime_stat) != 0) {
        error = "Host PipeWire runtime does not exist";
        return std::nullopt;
      }
      if (S_ISLNK(configured_runtime_stat.st_mode)) {
        error = "Host PipeWire runtime must not be a symbolic link";
        return std::nullopt;
      }
      std::error_code filesystem_error;
      const auto canonical_runtime {std::filesystem::canonical(runtime, filesystem_error)};
      if (filesystem_error || !std::filesystem::exists(canonical_runtime)) {
        error = "Host PipeWire runtime does not exist";
        return std::nullopt;
      }
      struct stat runtime_stat {};
      if (::lstat(canonical_runtime.c_str(), &runtime_stat) != 0 || !S_ISDIR(runtime_stat.st_mode)) {
        error = "Host PipeWire runtime is not a directory";
        return std::nullopt;
      }
      if (runtime_stat.st_uid != ::getuid()) {
        error = "Host PipeWire runtime is owned by another user";
        return std::nullopt;
      }
      if (path_is_within_runtime_root(canonical_runtime, private_base)) {
        error = "Host PipeWire runtime must not be inside the private Wayland runtime";
        return std::nullopt;
      }
      return canonical_runtime;
    }

    /**
     * @brief Verify that the host PipeWire socket is owned by the service user and connectable.
     *
     * @param runtime Canonical host user runtime directory.
     * @param remote PipeWire remote socket name.
     * @param failure Receives a concise launch failure reason.
     * @return True when a fresh UNIX socket connection succeeds.
     */
    bool verify_host_pipewire_socket(const std::filesystem::path &runtime, const std::string_view remote, std::string &failure) {
      const auto socket_path {runtime / remote};
      struct stat socket_stat {};
      if (::lstat(socket_path.c_str(), &socket_stat) != 0) {
        failure = "Host PipeWire socket does not exist";
        const int socket_error {errno};
        BOOST_LOG(error) << "SESSION_EVENT pipewire_socket_missing runtime=" << runtime << " remote=" << remote << " socket=" << socket_path << " errno=" << socket_error << " message=" << std::strerror(socket_error) << " uid=" << ::getuid();
        return false;
      }
      if (!S_ISSOCK(socket_stat.st_mode)) {
        failure = "Host PipeWire path is not a UNIX socket";
        BOOST_LOG(error) << "SESSION_EVENT pipewire_socket_wrong_type runtime=" << runtime << " remote=" << remote << " socket=" << socket_path << " mode=" << std::oct << socket_stat.st_mode << std::dec << " uid=" << ::getuid() << " socket_uid=" << socket_stat.st_uid;
        return false;
      }
      if (socket_stat.st_uid != ::getuid()) {
        failure = "Host PipeWire socket is owned by another user";
        BOOST_LOG(error) << "SESSION_EVENT pipewire_socket_wrong_owner runtime=" << runtime << " remote=" << remote << " socket=" << socket_path << " uid=" << ::getuid() << " socket_uid=" << socket_stat.st_uid;
        return false;
      }
      if (socket_path.string().size() >= sizeof(sockaddr_un::sun_path)) {
        failure = "Host PipeWire socket path is too long";
        return false;
      }
      const int socket_fd {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      if (socket_fd < 0) {
        failure = "Host PipeWire socket connection failed";
        BOOST_LOG(error) << "SESSION_EVENT pipewire_socket_connect_failed runtime=" << runtime << " remote=" << remote << " socket=" << socket_path << " errno=" << errno << " uid=" << ::getuid() << " socket_uid=" << socket_stat.st_uid;
        return false;
      }
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
      const auto address_length {static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1)};
      if (::connect(socket_fd, reinterpret_cast<const sockaddr *>(&address), address_length) != 0) {
        const int connect_error {errno};
        ::close(socket_fd);
        failure = "Host PipeWire socket connection failed";
        BOOST_LOG(error) << "SESSION_EVENT pipewire_socket_connect_failed runtime=" << runtime << " remote=" << remote << " socket=" << socket_path << " errno=" << connect_error << " message=" << std::strerror(connect_error) << " uid=" << ::getuid() << " socket_uid=" << socket_stat.st_uid;
        return false;
      }
      ::close(socket_fd);
      BOOST_LOG(info) << "SESSION_EVENT pipewire_endpoint_resolved host_runtime=" << runtime << " pipewire_socket=" << socket_path << " pipewire_remote=" << remote << " socket_uid=" << socket_stat.st_uid;
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
    manager.migration_required = false;
    manager.app_launch_rejected_reason.clear();
    manager.app_launch_rejected_message.clear();
    manager.steam_location = "unknown";
    const bool physical_output_connected {host_physical_output_connected()};
    const bool active_crtc_present {host_active_crtc_present()};
    const bool capturable_output_present {physical_desktop_capturable(
      physical_output_connected,
      active_crtc_present,
      physical_compositor_capture_available.load(std::memory_order_acquire)
    )};
    const bool verified_existing_gamescope {verified_game_mode_source_present()};
    const auto request {normalize_display_request(launch_session.width, launch_session.height, launch_session.fps, config::steamos_virtual_display.default_width, config::steamos_virtual_display.default_height, config::steamos_virtual_display.default_fps)};
#if defined(__linux__)
    const bool retained_owned_session {
      manager.origin == session_origin_e::owned_private &&
      manager.process_owned &&
      manager.process_group > 0 &&
      manager.current == state_e::Ready &&
      manager.width == request.width &&
      manager.height == request.height &&
      manager.fps == request.fps &&
      manager.hdr == launch_session.enable_hdr &&
      process_group_exists(manager.process_group) &&
      owned_wayland_socket_exists(manager.runtime_directory / "gamescope-0")
    };
#else
    const bool retained_owned_session {false};
#endif
    const auto decision {decide_virtual_display({
      .feature_enabled = config::steamos_virtual_display.enabled,
      .mode = config::steamos_virtual_display.mode,
      .physical_output_connected = physical_output_connected,
      .active_crtc_present = active_crtc_present,
      .capturable_output_present = capturable_output_present,
      .existing_owned_session = retained_owned_session,
      .host_supported = true,
      .verified_existing_gamescope_present = verified_existing_gamescope,
      .existing_gamescope_required = config::steamos_virtual_display.session_source == session_source_policy_e::existing_gamescope,
    })};
    manager.selection_reason = decision.reason;
    if (!decision.required) {
      if (manager.origin != session_origin_e::none) {
        BOOST_LOG(info) << "SESSION_EVENT source_released reason=desktop_capture_selected"
                        << " origin=" << to_string(manager.origin)
                        << " process_owned=" << (manager.process_owned ? "true" : "false");
        recover_failed_session_locked();
      }
      manager.current = state_e::Disabled;
      return true;
    }
    BOOST_LOG(info) << "SESSION_EVENT mode_decided mode=" << to_string(config::steamos_virtual_display.mode)
                    << " reason=" << decision.reason
                    << " physical_output_connected=" << (physical_output_connected ? "true" : "false")
                    << " active_crtc_present=" << (active_crtc_present ? "true" : "false")
                    << " verified_existing_gamescope=" << (verified_existing_gamescope ? "true" : "false");
#if !defined(__linux__)
    error = "SteamOS virtual display is only available on Linux";
    manager.current = state_e::Disabled;
    return false;
#else
    if (decision.reason == "owned_session_active") {
      manager.selection_reason = "retained_owned_private";
      manager.packet_tracking.store(false, std::memory_order_release);
      manager.encoded_packets.store(0, std::memory_order_relaxed);
      manager.encoded_bytes.store(0, std::memory_order_relaxed);
      manager.idr_packets.store(0, std::memory_order_relaxed);
      manager.captured_frames.store(0, std::memory_order_relaxed);
      BOOST_LOG(info) << "GAMESCOPE_SOURCE_REUSED origin=owned_private pid=" << manager.process_group
                      << " width=" << request.width << " height=" << request.height
                      << " fps=" << request.fps << " hdr=" << (launch_session.enable_hdr ? "true" : "false");
      return true;
    }
    if (manager.current == state_e::Ready && manager.origin != session_origin_e::none) {
      BOOST_LOG(info) << "SESSION_EVENT source_released reason=source_selection_changed"
                      << " origin=" << to_string(manager.origin)
                      << " process_owned=" << (manager.process_owned ? "true" : "false");
      recover_failed_session_locked();
    }
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
    const auto pipewire_runtime {validate_host_pipewire_runtime(host_pipewire_runtime(runtime_root), base, error)};
    if (!pipewire_runtime) {
      manager.current = state_e::Failed;
      return false;
    }
    const auto pipewire_remote {host_pipewire_remote()};
    if (!is_valid_pipewire_remote(pipewire_remote)) {
      error = "SteamOS host PipeWire remote must be a socket name";
      manager.current = state_e::Failed;
      return false;
    }
    if (!verify_host_pipewire_socket(*pipewire_runtime, pipewire_remote, error)) {
      manager.current = state_e::Failed;
      return false;
    }
    if (config::steamos_virtual_display.session_source != session_source_policy_e::owned_private) {
      std::string discovery_error;
      const auto discovery_timeout {std::min(std::chrono::milliseconds {500}, std::chrono::milliseconds {config::steamos_virtual_display.pipewire_node_timeout_milliseconds})};
      const auto sources {gamescope_source::discover_gamescope_sources(pipewire_runtime->string(), pipewire_remote, discovery_timeout, discovery_error)};
      gamescope_source::source_selection_request_t selection_request {
        .policy = config::steamos_virtual_display.session_source,
        .required_render_node = gpu->render_node,
      };
      if (config::steamos_virtual_display.existing_gamescope_pid > 0) {
        selection_request.explicit_gamescope_pid = config::steamos_virtual_display.existing_gamescope_pid;
      }
      const auto selected {gamescope_source::select_gamescope_source(sources, selection_request)};
      if (selected.has_value()) {
        const steam_session::target_session_t target {
          .gamescope_pid = selected->producer_pid,
          .cgroup = steam_session::cgroup_for_process(selected->producer_pid),
        };
        const auto resident_environment {steam_session::verified_resident_environment(target)};
        if (!resident_environment) {
          manager.app_launch_rejected_reason = "resident_steam_environment_unavailable";
          manager.app_launch_rejected_message = "A verified resident Steam environment is required for the selected Game Mode session";
          error = manager.app_launch_rejected_message;
          manager.current = state_e::Failed;
          return false;
        }
        const uint64_t display_generation {next_display_generation.fetch_add(1, std::memory_order_relaxed)};
        const auto display_endpoint {attached_display_endpoint(*resident_environment, selected->producer_pid, selected->producer_start_time, display_generation, pipewire_runtime->string(), pipewire_remote)};
        if (display_endpoint.verification != display_verification_e::verified) {
          manager.display_endpoint = display_endpoint;
          manager.app_launch_rejected_reason = display_endpoint.error;
          manager.app_launch_rejected_message = "The resident Steam Xwayland endpoint failed identity validation";
          error = manager.app_launch_rejected_message + " (" + display_endpoint.error + ')';
          manager.current = state_e::Failed;
          return false;
        }
        manager.selection_reason = "verified_existing_gamescope";
        manager.origin = session_origin_e::attached_existing;
        manager.process_owned = false;
        manager.runtime_owned = false;
        manager.runtime_directory.clear();
        manager.pipewire_runtime = pipewire_runtime->string();
        manager.pipewire_remote = pipewire_remote;
        manager.pipewire_node_id = selected->node_id;
        manager.pipewire_object_serial = selected->object_serial;
        manager.pipewire_producer_pid = selected->producer_pid;
        manager.pulse_runtime = (*pipewire_runtime / "pulse").string();
        manager.width = request.width;
        manager.height = request.height;
        manager.fps = request.fps;
        manager.hdr = launch_session.enable_hdr;
        manager.pci_bdf = gpu->pci_bdf;
        manager.render_node = gpu->render_node;
        manager.source_description = selected->node_description.empty() ? selected->application_name : selected->node_description;
        manager.source_executable = selected->executable;
        manager.source_process_start_time = selected->producer_start_time;
        manager.display_endpoint = display_endpoint;
        manager.presentation = desired_presentation(manager.origin);
        manager.steam_location = std::string {steam_session::to_string(steam_session::classify_current_user_instance({
          .gamescope_pid = selected->producer_pid,
          .cgroup = steam_session::cgroup_for_process(selected->producer_pid),
        }))};
        manager.packet_tracking.store(false, std::memory_order_release);
        manager.encoded_packets.store(0, std::memory_order_relaxed);
        manager.encoded_bytes.store(0, std::memory_order_relaxed);
        manager.idr_packets.store(0, std::memory_order_relaxed);
        manager.captured_frames.store(0, std::memory_order_relaxed);
  #if defined(__linux__)
        manager.process_group = selected->producer_pid;
  #endif
        manager.current = state_e::WaitingForCapture;
        BOOST_LOG(info) << "SESSION_DISPLAY_ENDPOINT_READY origin=attached_existing display=" << manager.display_endpoint.x11_display << " wayland=" << manager.display_endpoint.wayland_display << " generation=" << display_generation << " pid=" << selected->producer_pid;
        BOOST_LOG(info) << "GAMESCOPE_SOURCE_ATTACHED origin=attached_existing pid=" << selected->producer_pid << " start_time=" << selected->producer_start_time << " node_id=" << selected->node_id << " object_serial=" << selected->object_serial << " render_node=" << selected->render_node << " steam_location=" << manager.steam_location;
        return true;
      }
      BOOST_LOG(info) << "GAMESCOPE_SOURCE_UNAVAILABLE policy=" << to_string(config::steamos_virtual_display.session_source) << " reason=" << (discovery_error.empty() ? "selection_failed" : discovery_error);
      if (selected.error() == gamescope_source::source_error_e::ambiguous) {
        error = "Multiple verified existing Gamescope sources are available; select one explicit PID";
        manager.current = state_e::Failed;
        return false;
      }
      if (selected.error() == gamescope_source::source_error_e::explicit_pid_invalid) {
        error = "The configured existing Gamescope PID is unavailable or failed identity validation";
        manager.current = state_e::Failed;
        return false;
      }
      if (config::steamos_virtual_display.session_source == session_source_policy_e::existing_gamescope || verified_existing_gamescope) {
        error = verified_existing_gamescope ?
                  "A verified Game Mode Gamescope exists but no unique same-GPU capture source is available" :
                  "No unique verified existing Gamescope source is available";
        manager.current = state_e::Failed;
        return false;
      }
    }
    manager.runtime_directory = base / ("session-" + std::to_string(::getpid()) + "-" + std::to_string(launch_session.id));
    manager.selection_reason = "new_owned_private";
    manager.origin = session_origin_e::owned_private;
    manager.process_owned = true;
    manager.runtime_owned = true;
    manager.presentation = desired_presentation(manager.origin);
    manager.source_description = "SteamShine-owned private Gamescope";
    manager.source_executable = config::steamos_virtual_display.gamescope_path;
    manager.source_process_start_time = 0;
    const uint64_t display_generation {next_display_generation.fetch_add(1, std::memory_order_relaxed)};
    manager.display_endpoint = {
      .origin = session_origin_e::owned_private,
      .generation = display_generation,
      .verification = display_verification_e::unavailable,
      .error = "xwayland_display_missing",
    };
    manager.pci_bdf = gpu->pci_bdf;
    manager.render_node = gpu->render_node;
    manager.pipewire_runtime = pipewire_runtime->string();
    manager.pipewire_remote = pipewire_remote;
    manager.pipewire_node_id.reset();
    manager.pipewire_object_serial.reset();
    manager.pipewire_producer_pid = -1;
    manager.pulse_runtime = (*pipewire_runtime / "pulse").string();
    manager.width = request.width;
    manager.height = request.height;
    manager.fps = request.fps;
    manager.hdr = launch_session.enable_hdr;
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
    const auto xauthority {create_owned_xauthority(manager.runtime_directory, error)};
    if (!xauthority) {
      std::filesystem::remove_all(manager.runtime_directory, ec);
      manager.current = state_e::Failed;
      return false;
    }
    manager.current = state_e::Starting;
    std::error_code executable_error;
    const auto current_executable {std::filesystem::read_symlink("/proc/self/exe", executable_error)};
    if (executable_error || current_executable.empty()) {
      std::filesystem::remove_all(manager.runtime_directory, ec);
      error = "Failed to resolve the SteamShine session bootstrap executable";
      manager.current = state_e::Failed;
      return false;
    }
    const std::string bootstrap_executable {
      std::getenv("STEAMSHINE_SESSION_BOOTSTRAP") ? std::getenv("STEAMSHINE_SESSION_BOOTSTRAP") : current_executable.string()
    };
    const pid_t child {::fork()};
    if (child == 0) {
      ::setpgid(0, 0);
      const auto path {config::steamos_virtual_display.gamescope_path};
      const auto runtime {manager.runtime_directory.string()};
      const auto xauthority_value {xauthority->string()};
      const auto pipewire_runtime_value {pipewire_runtime->string()};
      ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
      ::setenv("PIPEWIRE_RUNTIME_DIR", pipewire_runtime_value.c_str(), 1);
      ::setenv("PIPEWIRE_REMOTE", pipewire_remote.c_str(), 1);
      ::setenv("PULSE_RUNTIME_PATH", manager.pulse_runtime.c_str(), 1);
      ::setenv("XAUTHORITY", xauthority_value.c_str(), 1);
      // A headless Gamescope owns its Wayland server.  Inheriting the desktop
      // display name makes Gamescope try to connect to a non-existent parent
      // socket below this private runtime directory before it starts that
      // server.
      ::unsetenv("WAYLAND_DISPLAY");
      ::unsetenv("DISPLAY");
      // Gamescope owns the nested Wayland session. Do not inherit the desktop
      // session type because it can make a headless compositor select a host
      // session backend rather than its private runtime.
      ::unsetenv("XDG_SESSION_TYPE");
      std::vector<char *> argv;
      const auto generation {std::to_string(display_generation)};
      argv.reserve(arguments.size() + 6);
      argv.push_back(const_cast<char *>(path.c_str()));
      for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
      }
      argv.push_back(const_cast<char *>("--"));
      argv.push_back(const_cast<char *>(bootstrap_executable.c_str()));
      argv.push_back(const_cast<char *>("--steamshine-session-bootstrap"));
      argv.push_back(const_cast<char *>(runtime.c_str()));
      argv.push_back(const_cast<char *>(generation.c_str()));
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
    if (const auto identity {gamescope_source::read_process_identity(child)}) {
      manager.source_process_start_time = identity->start_time;
      manager.source_executable = identity->executable.string();
      manager.steam_location = std::string {steam_session::to_string(steam_session::classify_current_user_instance({
        .gamescope_pid = child,
        .runtime_directory = manager.runtime_directory.string(),
        .wayland_display = "gamescope-0",
        .cgroup = steam_session::cgroup_for_process(child),
      }))};
    }
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
        manager.display_endpoint = read_owned_display_endpoint(manager.runtime_directory, child, manager.source_process_start_time, display_generation, manager.pipewire_runtime, manager.pipewire_remote, manager.pulse_runtime);
        if (manager.display_endpoint.verification == display_verification_e::verified) {
          manager.current = state_e::WaitingForCapture;
          BOOST_LOG(info) << "SESSION_DISPLAY_ENDPOINT_READY origin=owned_private display=" << manager.display_endpoint.x11_display << " wayland=" << manager.display_endpoint.wayland_display << " generation=" << display_generation << " pid=" << child;
          BOOST_LOG(info) << "SteamOS virtual display socket ready: " << request.width << 'x' << request.height << '@' << request.fps << " on AMD PCI " << manager.pci_bdf << " (" << manager.render_node << ')';
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (error.empty()) {
      const auto reason {manager.display_endpoint.error.empty() ? std::string {"xwayland_display_missing"} : manager.display_endpoint.error};
      manager.app_launch_rejected_reason = reason;
      manager.app_launch_rejected_message = "Gamescope did not publish a verified Xwayland endpoint";
      error = "Timed out waiting for a verified Gamescope display endpoint (" + reason + ')';
      manager.current = state_e::Failed;
    }
    stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
    std::filesystem::remove_all(manager.runtime_directory, ec);
    manager.process_group = -1;
    return false;
#endif
  }

  bool capture_backend_required() {
    const bool physical_output_connected {host_physical_output_connected()};
    const bool active_crtc_present {host_active_crtc_present()};
    const bool capturable_output_present {physical_desktop_capturable(
      physical_output_connected,
      active_crtc_present,
      physical_compositor_capture_available.load(std::memory_order_acquire)
    )};
    return decide_virtual_display({
                                    .feature_enabled = config::steamos_virtual_display.enabled,
                                    .mode = config::steamos_virtual_display.mode,
                                    .physical_output_connected = physical_output_connected,
                                    .active_crtc_present = active_crtc_present,
                                    .capturable_output_present = capturable_output_present,
                                    .existing_owned_session = active(),
                                    .host_supported = true,
                                  })
      .required;
  }

  bool physical_output_connected() {
    return host_physical_output_connected();
  }

  void set_physical_compositor_capture_available(const bool available) {
    physical_compositor_capture_available.store(available, std::memory_order_release);
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
    snapshot.origin = manager.origin;
    snapshot.process_owned = manager.process_owned;
    snapshot.runtime_owned = manager.runtime_owned;
    snapshot.source_description = manager.source_description;
    snapshot.source_executable = manager.source_executable;
    snapshot.source_process_start_time = manager.source_process_start_time;
    snapshot.steam_location = manager.steam_location;
    snapshot.migration_required = manager.migration_required;
    snapshot.app_launch_rejected_reason = manager.app_launch_rejected_reason;
    snapshot.app_launch_rejected_message = manager.app_launch_rejected_message;
    snapshot.display_endpoint = manager.display_endpoint;
    snapshot.selection_reason = manager.selection_reason;
    snapshot.presentation = manager.presentation;
    snapshot.local_presenter_active = manager.local_presenter_active;
    snapshot.local_presented_frames = manager.local_presented_frames.load(std::memory_order_relaxed);
    snapshot.local_dropped_frames = manager.local_dropped_frames.load(std::memory_order_relaxed);
    snapshot.runtime_directory = manager.runtime_directory.string();
    snapshot.pci_bdf = manager.pci_bdf;
    snapshot.render_node = manager.render_node;
    snapshot.pipewire_runtime = manager.pipewire_runtime;
    snapshot.pipewire_remote = manager.pipewire_remote;
    snapshot.pipewire_node_id = manager.pipewire_node_id;
    snapshot.pipewire_object_serial = manager.pipewire_object_serial;
    snapshot.pipewire_producer_pid = manager.pipewire_producer_pid;
    snapshot.width = manager.width;
    snapshot.height = manager.height;
    snapshot.fps = manager.fps;
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

  void mark_streaming_disconnected() {
    bool stop_owned_session {};
    {
      std::scoped_lock lock {manager.mutex};
      manager.stream_requested = false;
      manager.packet_tracking.store(false, std::memory_order_release);
      stop_owned_session = manager.origin == session_origin_e::owned_private && manager.process_owned && !config::steamos_virtual_display.keep_session_alive;
      if (manager.current == state_e::Streaming && !stop_owned_session) {
        manager.current = state_e::Ready;
        BOOST_LOG(info) << "SteamOS virtual display retained after stream disconnect origin=" << (manager.origin == session_origin_e::owned_private ? "owned_private" : "attached_existing");
      }
    }
    if (stop_owned_session) {
      BOOST_LOG(info) << "SteamOS virtual display stopping owned session after stream disconnect";
      stop();
    }
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

  int run_display_endpoint_bootstrap(const std::string_view report_directory, const uint64_t generation) {
#if defined(__linux__)
    const auto *const inherited_runtime {std::getenv("XDG_RUNTIME_DIR")};
    const std::filesystem::path runtime {report_directory};
    struct stat runtime_stat {};
    if (!inherited_runtime || runtime.empty() || runtime.string() != inherited_runtime || ::lstat(runtime.c_str(), &runtime_stat) != 0 || !S_ISDIR(runtime_stat.st_mode) || runtime_stat.st_uid != ::getuid()) {
      return 3;
    }
    const int directory {::open(runtime.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (directory < 0) {
      return 3;
    }
    const auto close_directory {util::fail_guard([directory]() {
      ::close(directory);
    })};
    const auto environment_value = [](const char *name) {
      const auto *const value {std::getenv(name)};
      return value ? std::string {value} : std::string {};
    };
    const auto identity {gamescope_source::read_process_identity(::getpid())};
    boost::property_tree::ptree report;
    report.put("owner", "steamshine");
    report.put("generation", generation);
    report.put("bootstrap_pid", ::getpid());
    report.put("bootstrap_start_time", identity ? identity->start_time : 0);
    report.put("xdg_runtime_directory", runtime.string());
    report.put("wayland_display", environment_value("WAYLAND_DISPLAY"));
    report.put("gamescope_wayland_display", environment_value("GAMESCOPE_WAYLAND_DISPLAY"));
    report.put("display", environment_value("DISPLAY"));
    report.put("xauthority", environment_value("XAUTHORITY"));
    report.put("dbus_session_bus_address", environment_value("DBUS_SESSION_BUS_ADDRESS"));
    std::ostringstream serialized;
    boost::property_tree::write_json(serialized, report, false);
    const std::string contents {serialized.str()};
    const std::string temporary_name {".display-endpoint-" + std::to_string(::getpid()) + ".tmp"};
    const int output {::openat(directory, temporary_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (output < 0) {
      return 4;
    }
    size_t written {};
    while (written < contents.size()) {
      const auto result {::write(output, contents.data() + written, contents.size() - written)};
      if (result <= 0) {
        ::close(output);
        ::unlinkat(directory, temporary_name.c_str(), 0);
        return 4;
      }
      written += static_cast<size_t>(result);
    }
    const bool flushed {::fsync(output) == 0};
    ::close(output);
    if (!flushed || ::renameat(directory, temporary_name.c_str(), directory, display_endpoint_report_name.data()) != 0 || ::fsync(directory) != 0) {
      ::unlinkat(directory, temporary_name.c_str(), 0);
      return 4;
    }
    for (;;) {
      ::pause();
    }
#else
    (void) report_directory;
    (void) generation;
    return 3;
#endif
  }

  std::optional<session_display_endpoint_t> application_environment() {
    std::scoped_lock lock {manager.mutex};
    if (manager.origin == session_origin_e::none || manager.display_endpoint.verification != display_verification_e::verified || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      return std::nullopt;
    }
    const auto identity {gamescope_source::read_process_identity(manager.display_endpoint.producer_pid)};
    if (!identity || identity->start_time != manager.display_endpoint.producer_start_time) {
      return std::nullopt;
    }
    const auto environment_identity {gamescope_source::read_process_identity(manager.display_endpoint.environment_source_pid)};
    if (!environment_identity || environment_identity->start_time != manager.display_endpoint.environment_source_start_time) {
      return std::nullopt;
    }
    return manager.display_endpoint;
  }

  bool steam_command_allowed(const std::string_view command, std::string &error_message) {
    if (!steam_session::command_references_steam(command)) {
      return true;
    }
    steam_session::target_session_t target;
    {
      std::scoped_lock lock {manager.mutex};
      if (manager.origin == session_origin_e::none || manager.process_group <= 0) {
        return true;
      }
      target = {
        .gamescope_pid = manager.process_group,
        .runtime_directory = manager.display_endpoint.xdg_runtime_directory,
        .wayland_display = manager.display_endpoint.wayland_display,
        .cgroup = steam_session::cgroup_for_process(manager.process_group),
      };
    }
    const auto location {steam_session::classify_current_user_instance(target)};
    {
      std::scoped_lock lock {manager.mutex};
      manager.steam_location = std::string {steam_session::to_string(location)};
    }
    if (location == steam_session::instance_location_e::outside_target_gamescope) {
      error_message = "Steam is already running outside the SteamShine Gamescope session; stop or migrate it before launching Steam here";
      bool migration_required {};
      {
        std::scoped_lock lock {manager.mutex};
        migration_required = manager.origin == session_origin_e::owned_private;
        manager.migration_required = migration_required;
        manager.app_launch_rejected_reason = "steam_outside_target_gamescope";
        manager.app_launch_rejected_message = error_message;
      }
      BOOST_LOG(error) << "APP_LAUNCH_REJECTED reason=steam_outside_target_gamescope"
                       << " migration_required=" << (migration_required ? "true" : "false")
                       << " steam_location=outside_target_gamescope";
      return false;
    }
    if (location == steam_session::instance_location_e::unknown) {
      error_message = "Steam placement could not be verified; refusing to risk a second Steam instance";
      {
        std::scoped_lock lock {manager.mutex};
        manager.migration_required = false;
        manager.app_launch_rejected_reason = "steam_placement_unverified";
        manager.app_launch_rejected_message = error_message;
      }
      BOOST_LOG(error) << "APP_LAUNCH_REJECTED reason=steam_placement_unverified"
                       << " migration_required=false steam_location=unknown";
      return false;
    }
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

  bool gamescope_pipewire_endpoint(std::string &runtime_directory, std::string &remote_name, int &gamescope_pid) {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    if (manager.pipewire_runtime.empty() || manager.pipewire_remote.empty() || manager.process_group <= 0 || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      return false;
    }
    runtime_directory = manager.pipewire_runtime;
    remote_name = manager.pipewire_remote;
    gamescope_pid = manager.process_group;
    return true;
#else
    (void) runtime_directory;
    (void) remote_name;
    (void) gamescope_pid;
    return false;
#endif
  }

  bool gamescope_input_required() {
    std::scoped_lock lock {manager.mutex};
    return manager.origin != session_origin_e::none &&
           manager.process_group > 0 &&
           (manager.current == state_e::WaitingForCapture || manager.current == state_e::Ready || manager.current == state_e::Streaming);
  }

  bool gamescope_input_endpoint(std::string &socket_path, std::string &error) {
#if defined(__linux__)
    std::scoped_lock lock {manager.mutex};
    if (manager.process_group <= 0 || manager.source_process_start_time == 0 || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      error = "gamescope_input_source_unavailable";
      return false;
    }
    const auto identity {gamescope_source::read_process_identity(manager.process_group)};
    if (!identity || identity->start_time != manager.source_process_start_time) {
      error = "gamescope_input_identity_changed";
      return false;
    }
    const std::filesystem::path runtime {manager.display_endpoint.xdg_runtime_directory};
    const auto socket {gamescope_eis_socket_path(runtime, manager.display_endpoint.gamescope_wayland_display)};
    if (!socket) {
      error = "gamescope_input_runtime_unavailable";
      return false;
    }
    struct stat socket_status {};
    if (::lstat(socket->c_str(), &socket_status) != 0 || !S_ISSOCK(socket_status.st_mode) || socket_status.st_uid != ::getuid()) {
      error = "gamescope_input_eis_socket_invalid";
      return false;
    }
    if (manager.verified_input_socket == *socket &&
        manager.verified_input_socket_device == static_cast<std::uint64_t>(socket_status.st_dev) &&
        manager.verified_input_socket_inode == static_cast<std::uint64_t>(socket_status.st_ino) &&
        manager.verified_input_producer_pid == manager.process_group &&
        manager.verified_input_producer_start_time == manager.source_process_start_time) {
      socket_path = socket->string();
      error.clear();
      return true;
    }
    const auto peer_pid {unix_socket_peer_pid(*socket)};
    if (!peer_pid || *peer_pid != manager.process_group) {
      error = "gamescope_input_eis_socket_unverified";
      return false;
    }
    manager.verified_input_socket = *socket;
    manager.verified_input_socket_device = static_cast<std::uint64_t>(socket_status.st_dev);
    manager.verified_input_socket_inode = static_cast<std::uint64_t>(socket_status.st_ino);
    manager.verified_input_producer_pid = manager.process_group;
    manager.verified_input_producer_start_time = manager.source_process_start_time;
    socket_path = socket->string();
    error.clear();
    return true;
#else
    (void) socket_path;
    error = "gamescope_input_unsupported";
    return false;
#endif
  }

  bool open_verified_gamescope_pipewire_consumer(pipewire_capture::stream_descriptor_t &descriptor, std::string &error) {
#if defined(__linux__)
    std::string runtime;
    std::string remote;
    int producer_pid {};
    uint64_t producer_start_time {};
    uint64_t object_serial {};
    std::string render_node;
    std::string source_label;
    {
      std::scoped_lock lock {manager.mutex};
      if (manager.pipewire_runtime.empty() || manager.pipewire_remote.empty() || manager.process_group <= 0 || manager.source_process_start_time == 0 || manager.render_node.empty() || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
        error = "Selected Gamescope PipeWire source is unavailable";
        return false;
      }
      runtime = manager.pipewire_runtime;
      remote = manager.pipewire_remote;
      producer_pid = manager.process_group;
      producer_start_time = manager.source_process_start_time;
      object_serial = manager.pipewire_object_serial.value_or(0);
      render_node = manager.render_node;
      source_label = manager.source_description;
    }
    const auto timeout {std::chrono::milliseconds {config::steamos_virtual_display.pipewire_node_timeout_milliseconds}};
    const auto sources {gamescope_source::discover_gamescope_sources(runtime, remote, timeout, error)};
    const auto source {std::find_if(sources.begin(), sources.end(), [producer_pid, producer_start_time, object_serial, &render_node](const gamescope_source::gamescope_source_t &candidate) {
      return candidate.producer_pid == producer_pid && candidate.producer_start_time == producer_start_time && pipewire_capture::matches_selected_render_node(candidate.render_node, render_node) &&
             (object_serial == 0 || candidate.object_serial == object_serial) && gamescope_source::source_identity_is_current(candidate);
    })};
    if (source == sources.end()) {
      if (error.empty()) {
        error = "Verified Gamescope PipeWire source disappeared or changed identity";
      }
      return false;
    }
    const auto fd {gamescope_source::open_host_pipewire_socket(runtime, remote, error)};
    if (!fd) {
      return false;
    }
    descriptor = {
      .connected_core_fd = *fd,
      .node_id = source->node_id,
      .object_serial = source->object_serial,
      .producer_pid = source->producer_pid,
      .producer_start_time = source->producer_start_time,
      .render_node = render_node,
      .source_label = source_label.empty() ? source->node_description : source_label,
    };
    if (!pipewire_capture::has_verified_source_identity(descriptor)) {
      close(descriptor.connected_core_fd);
      descriptor.connected_core_fd = -1;
      error = "Verified Gamescope PipeWire source has incomplete identity";
      return false;
    }
    return true;
#else
    (void) descriptor;
    error = "PipeWire consumers are available only on Linux";
    return false;
#endif
  }

  void mark_gamescope_pipewire_node(const uint32_t node_id, const uint64_t object_serial, const int producer_pid) {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    if (node_id == 0 || object_serial == 0 || producer_pid <= 0 || producer_pid != manager.process_group || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      return;
    }
    manager.pipewire_node_id = node_id;
    manager.pipewire_object_serial = object_serial;
    manager.pipewire_producer_pid = producer_pid;
#else
    (void) node_id;
    (void) object_serial;
    (void) producer_pid;
#endif
  }

  bool active() {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    return manager.process_group > 0 && (manager.origin == session_origin_e::attached_existing || !manager.runtime_directory.empty()) &&
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
    if (manager.process_owned && manager.process_group > 0) {
      manager.current = state_e::Stopping;
      manager.packet_tracking.store(false, std::memory_order_release);
      BOOST_LOG(info) << "SteamOS virtual display stopping owned Gamescope session";
      BOOST_LOG(info) << "SteamOS virtual display encoded packets=" << manager.encoded_packets.load(std::memory_order_relaxed)
                      << " bytes=" << manager.encoded_bytes.load(std::memory_order_relaxed)
                      << " idr=" << manager.idr_packets.load(std::memory_order_relaxed)
                      << " captured_frames=" << manager.captured_frames.load(std::memory_order_relaxed);
      stop_owned_process_group(manager.process_group, std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds});
      manager.process_group = -1;
    } else if (manager.origin == session_origin_e::attached_existing) {
      BOOST_LOG(info) << "GAMESCOPE_SOURCE_DETACHED origin=attached_existing pid=" << manager.process_group << " process_owned=false runtime_owned=false";
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
