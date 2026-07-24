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
#if defined(__linux__)
      pid_t process_group {-1};  ///< Process group containing Gamescope and its children.
#endif
    } manager;

    constexpr std::string_view owner_marker_name {"steamshine-owner"};
    constexpr std::string_view owner_marker_contents {"steamshine-steamos-virtual-session-v1\n"};

    /**
     * @brief Clamp a requested dimension or FPS to a safe Gamescope range.
     *
     * @param value Client-provided value.
     * @param fallback Configured fallback value.
     * @param minimum Inclusive supported minimum.
     * @param maximum Inclusive supported maximum.
     * @return Normalized value.
     */
    int normalize(const int value, const int fallback, const int minimum, const int maximum) {
      return std::clamp(value > 0 ? value : fallback, minimum, maximum);
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
     * @brief Resolve a DRM render node to an AMD GPU descriptor.
     *
     * @param render_node Candidate `/dev/dri/renderD*` node.
     * @return AMD descriptor, or no value when the node is not an AMD device.
     */
    std::optional<gpu_candidate_t> amd_gpu_from_render_node(const std::filesystem::path &render_node) {
      const auto sys_device {std::filesystem::path {"/sys/class/drm"} / render_node.filename() / "device"};
      const auto vendor {read_attribute(sys_device / "vendor")};
      const auto device {read_attribute(sys_device / "device")};
      if (vendor != "0x1002" || device.size() != 6 || !std::filesystem::exists(render_node)) {
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
      if (!requested.empty() && requested.find(':') != std::string::npos && requested.find('/') == std::string::npos && requested.find('.') == std::string::npos) {
        if (requested.rfind("1002:", 0) != 0) {
          error = "SteamOS virtual display requires an AMD GPU identifier";
          return std::nullopt;
        }
#ifdef SUNSHINE_TESTS
        // Unit tests use a synthetic Gamescope PCI identifier because CI has no DRM GPU.
        return gpu_candidate_t {"test-pci-bdf", "", "", requested, 0};
#else
        error = "Configure the SteamOS GPU as a PCI BDF or DRM render node";
        return std::nullopt;
#endif
      }
      if (!requested.empty()) {
        if (requested.find(':') != std::string::npos && requested.find('.') != std::string::npos) {
          std::error_code iterator_error;
          for (const auto &entry : std::filesystem::directory_iterator {"/dev/dri", iterator_error}) {
            const auto name {entry.path().filename().string()};
            if (name.rfind("renderD", 0) != 0) {
              continue;
            }
            const auto sys_device {std::filesystem::path {"/sys/class/drm"} / name / "device"};
            std::error_code canonical_error;
            if (std::filesystem::canonical(sys_device, canonical_error).filename() == requested) {
              if (const auto candidate {amd_gpu_from_render_node(entry.path())}) {
                return candidate;
              }
            }
          }
        }
        const auto explicit_node {std::filesystem::path {requested}};
        if (const auto candidate {amd_gpu_from_render_node(explicit_node)}) {
          return candidate;
        }
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

  std::vector<std::string> gamescope_arguments(const std::string &help_text, const int width, const int height, const int fps, const bool enable_hdr, const std::string &gpu_device, std::string &error) {
    const auto has_option {[&help_text](const std::string_view option) {
      return help_text.find(option) != std::string::npos;
    }};
    if (!has_option("--nested-width") || !has_option("--nested-height") || !has_option("--nested-refresh") || !has_option("--expose-wayland")) {
      error = "Installed Gamescope does not advertise nested Wayland display options";
      return {};
    }
    std::vector<std::string> arguments;
    if (has_option("--backend") && help_text.find("headless") != std::string::npos) {
      arguments.emplace_back("--backend");
      arguments.emplace_back("headless");
    } else if (has_option("--headless")) {
      arguments.emplace_back("--headless");
    } else {
      error = "Installed Gamescope does not advertise a headless backend";
      return {};
    }
    arguments.emplace_back("--nested-width");
    arguments.emplace_back(std::to_string(width));
    arguments.emplace_back("--nested-height");
    arguments.emplace_back(std::to_string(height));
    arguments.emplace_back("--nested-refresh");
    arguments.emplace_back(std::to_string(fps));
    arguments.emplace_back("--expose-wayland");
    if (has_option("--scaler")) {
      arguments.emplace_back("--scaler");
      arguments.emplace_back("fit");
    }
    if (enable_hdr) {
      if (!has_option("--hdr-enabled")) {
        error = "Client requested HDR but installed Gamescope does not advertise HDR output";
        return {};
      }
      arguments.emplace_back("--hdr-enabled");
    }
    if (!gpu_device.empty()) {
      if (!has_option("--prefer-vk-device")) {
        error = "Installed Gamescope does not advertise AMD Vulkan device selection";
        return {};
      }
      arguments.emplace_back("--prefer-vk-device");
      arguments.emplace_back(gpu_device);
    }
    return arguments;
  }

  bool prepare(const rtsp_stream::launch_session_t &launch_session, std::string &error) {
    std::scoped_lock lock {manager.mutex};
    if (!config::steamos_virtual_display.enabled) {
      manager.current = state_e::Disabled;
      return true;
    }
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
    const int width {normalize(launch_session.width, config::steamos_virtual_display.default_width, 640, 7680)};
    const int height {normalize(launch_session.height, config::steamos_virtual_display.default_height, 480, 4320)};
    const int fps {normalize(launch_session.fps, config::steamos_virtual_display.default_fps, 30, 240)};
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
    const auto arguments {gamescope_arguments(help_text, width, height, fps, launch_session.enable_hdr, gpu->gamescope_device, error)};
    if (arguments.empty()) {
      manager.current = state_e::Failed;
      return false;
    }
    const auto base {runtime_base()};
    if (base.empty() || !std::filesystem::exists(base.parent_path())) {
      error = "XDG_RUNTIME_DIR is unavailable; refusing persistent runtime fallback";
      manager.current = state_e::Failed;
      return false;
    }
    manager.runtime_directory = base / ("session-" + std::to_string(::getpid()) + "-" + std::to_string(launch_session.id));
    manager.pci_bdf = gpu->pci_bdf;
    manager.render_node = gpu->render_node;
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
    const auto socket {manager.runtime_directory / "wayland-0"};
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
        BOOST_LOG(info) << "SteamOS virtual display socket ready: " << width << 'x' << height << '@' << fps << " on AMD PCI " << manager.pci_bdf << " (" << manager.render_node << ')';
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
    return config::steamos_virtual_display.enabled;
  }

  void cleanup_orphan_sessions() {
#if defined(__linux__)
    if (!config::steamos_virtual_display.enabled || !config::steamos_virtual_display.cleanup_orphan_sessions) {
      return;
    }
    const auto base {runtime_base()};
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
    }
  }

  bool application_environment(std::string &runtime_directory, std::string &wayland_display) {
    std::scoped_lock lock {manager.mutex};
    if (manager.runtime_directory.empty() || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming)) {
      return false;
    }
    runtime_directory = manager.runtime_directory.string();
    wayland_display = "wayland-0";
    return true;
  }

  bool capture_socket(std::string &socket_path) {
    std::scoped_lock lock {manager.mutex};
    const auto socket {manager.runtime_directory / "wayland-0"};
    if (manager.runtime_directory.empty() || (manager.current != state_e::WaitingForCapture && manager.current != state_e::Ready && manager.current != state_e::Streaming) || !owned_wayland_socket_exists(socket)) {
      return false;
    }
    socket_path = socket.string();
    return true;
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
           (manager.current == state_e::WaitingForCapture || manager.current == state_e::Ready || manager.current == state_e::Streaming);
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

  void stop() {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    if (manager.process_group > 0) {
      manager.current = state_e::Stopping;
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
