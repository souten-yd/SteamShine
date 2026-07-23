/**
 * @file src/steamos_virtual_session.cpp
 * @brief SteamOS headless Gamescope session lifecycle implementation.
 */
#include "steamos_virtual_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "config.h"
#include "logging.h"
#include "rtsp.h"

#if defined(__linux__)
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
#if defined(__linux__)
      pid_t process_group {-1};  ///< Process group containing Gamescope and its children.
#endif
    } manager;

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
    bool supports_required_options(const std::string &executable) {
      int pipe_fds[2] {};
      if (::pipe(pipe_fds) != 0) {
        return false;
      }
      const pid_t child {::fork()};
      if (child == 0) {
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::execlp(executable.c_str(), executable.c_str(), "--help", nullptr);
        _exit(127);
      }
      ::close(pipe_fds[1]);
      std::string help;
      std::array<char, 4096> buffer {};
      while (help.size() < 65536) {
        const auto bytes {::read(pipe_fds[0], buffer.data(), buffer.size())};
        if (bytes <= 0) {
          break;
        }
        help.append(buffer.data(), static_cast<std::size_t>(bytes));
      }
      ::close(pipe_fds[0]);
      if (child > 0) {
        ::waitpid(child, nullptr, 0);
      }
      return child > 0 && help.find("--headless") != std::string::npos && help.find("--nested-width") != std::string::npos && help.find("--nested-height") != std::string::npos && help.find("--nested-refresh") != std::string::npos;
    }
#endif
  }  // namespace

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
    if (manager.current != state_e::Idle && manager.current != state_e::Disabled) {
      error = "A SteamShine virtual display session is already active";
      return false;
    }
    if (!supports_required_options(config::steamos_virtual_display.gamescope_path)) {
      error = "Installed Gamescope does not advertise the required headless options";
      manager.current = state_e::Failed;
      return false;
    }
    const auto base {runtime_base()};
    if (base.empty() || !std::filesystem::exists(base.parent_path())) {
      error = "XDG_RUNTIME_DIR is unavailable; refusing persistent runtime fallback";
      manager.current = state_e::Failed;
      return false;
    }
    const int width {normalize(launch_session.width, config::steamos_virtual_display.default_width, 640, 7680)};
    const int height {normalize(launch_session.height, config::steamos_virtual_display.default_height, 480, 4320)};
    const int fps {normalize(launch_session.fps, config::steamos_virtual_display.default_fps, 30, 240)};
    manager.runtime_directory = base / ("session-" + std::to_string(::getpid()) + "-" + std::to_string(launch_session.id));
    std::error_code ec;
    std::filesystem::create_directories(manager.runtime_directory, ec);
    if (ec) {
      error = "Failed to create owned virtual-session runtime directory";
      manager.current = state_e::Failed;
      return false;
    }
    const auto socket {manager.runtime_directory / "wayland-0"};
    manager.current = state_e::Starting;
    const pid_t child {::fork()};
    if (child == 0) {
      ::setpgid(0, 0);
      const auto path {config::steamos_virtual_display.gamescope_path};
      const auto runtime {manager.runtime_directory.string()};
      const auto width_s {std::to_string(width)};
      const auto height_s {std::to_string(height)};
      const auto fps_s {std::to_string(fps)};
      ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
      ::execlp(path.c_str(), path.c_str(), "--headless", "--nested-width", width_s.c_str(), "--nested-height", height_s.c_str(), "--nested-refresh", fps_s.c_str(), "--", "/bin/sh", "-c", "exec sleep infinity", nullptr);
      _exit(127);
    }
    if (child < 0) {
      std::filesystem::remove_all(manager.runtime_directory, ec);
      error = "Failed to fork Gamescope";
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
      if (std::filesystem::exists(socket)) {
        manager.current = state_e::Ready;
        BOOST_LOG(info) << "SteamOS virtual display ready: " << width << 'x' << height << '@' << fps;
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (error.empty()) {
      error = "Timed out waiting for the Gamescope Wayland socket";
      manager.current = state_e::Failed;
    }
    ::kill(-manager.process_group, SIGTERM);
    ::waitpid(child, nullptr, 0);
    std::filesystem::remove_all(manager.runtime_directory, ec);
    manager.process_group = -1;
    return false;
#endif
  }

  void mark_streaming() {
    std::scoped_lock lock {manager.mutex};
    if (manager.current == state_e::Ready) {
      manager.current = state_e::Streaming;
    }
  }

  void stop() {
    std::scoped_lock lock {manager.mutex};
#if defined(__linux__)
    if (manager.process_group > 0) {
      manager.current = state_e::Stopping;
      ::kill(-manager.process_group, SIGTERM);
      const auto deadline {std::chrono::steady_clock::now() + std::chrono::seconds {config::steamos_virtual_display.shutdown_timeout_seconds}};
      while (::waitpid(manager.process_group, nullptr, WNOHANG) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
      }
      ::kill(-manager.process_group, SIGKILL);
      ::waitpid(manager.process_group, nullptr, 0);
      manager.process_group = -1;
    }
#endif
    manager.current = state_e::Recovering;
    std::error_code ec;
    std::filesystem::remove_all(manager.runtime_directory, ec);
    manager.runtime_directory.clear();
    manager.current = config::steamos_virtual_display.enabled ? state_e::Idle : state_e::Disabled;
  }

  state_e state() {
    std::scoped_lock lock {manager.mutex};
    return manager.current;
  }
}  // namespace steamos_virtual_session
