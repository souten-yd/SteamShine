/**
 * @file src/platform/linux/host_desktop_endpoint.cpp
 * @brief Immutable host desktop endpoint implementation.
 */
#include "host_desktop_endpoint.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(SUNSHINE_BUILD_WAYLAND)
  #include <xdg-shell.h>

  #include <wayland-client.h>

  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace host_desktop_endpoint {
  namespace {
    std::mutex endpoint_mutex;  ///< Serializes the one-time endpoint capture.
    endpoint_t endpoint;  ///< Immutable host endpoint after capture.
    bool captured {};  ///< Whether the host endpoint was already captured.

    /**
     * @brief Read an environment variable without retaining its process-owned pointer.
     *
     * @param name Environment variable name.
     * @return Owned value, or an empty string when absent.
     */
    std::string environment_value(const char *name) {
      const auto *const value {std::getenv(name)};
      return value ? value : "";
    }
  }  // namespace

  void capture() {
    std::scoped_lock lock {endpoint_mutex};
    if (captured) {
      return;
    }
    endpoint = {
      .xdg_runtime_directory = environment_value("XDG_RUNTIME_DIR"),
      .wayland_display = environment_value("WAYLAND_DISPLAY"),
      .x11_display = environment_value("DISPLAY"),
    };
    captured = true;
  }

  endpoint_t current() {
    std::scoped_lock lock {endpoint_mutex};
    return endpoint;
  }

  bool supports_wayland_presentation(std::string &error) {
#if defined(SUNSHINE_BUILD_WAYLAND)
    const auto host {current()};
    if (host.xdg_runtime_directory.empty() || host.wayland_display.empty() || host.wayland_display.find('/') != std::string::npos) {
      error = "Captured host Wayland endpoint is unavailable";
      return false;
    }
    const auto socket_path {host.xdg_runtime_directory + "/" + host.wayland_display};
    if (socket_path.size() >= sizeof(sockaddr_un {}.sun_path)) {
      error = "Captured host Wayland socket path is too long";
      return false;
    }
    const int fd {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (fd < 0) {
      error = "Failed to create a host Wayland socket";
      return false;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
      ::close(fd);
      error = "Failed to connect to the captured host Wayland socket";
      return false;
    }
    wl_display *const display {wl_display_connect_to_fd(fd)};
    if (!display) {
      ::close(fd);
      error = "Failed to create a host Wayland client connection";
      return false;
    }
    struct registry_state_t {
      bool compositor {};  ///< Whether the host exposes `wl_compositor`.
      bool xdg_shell {};  ///< Whether the host exposes `xdg_wm_base`.
    } state;
    const wl_registry_listener listener {
      .global = [](void *data, wl_registry *, uint32_t, const char *interface, uint32_t) {
        auto &registry_state {*static_cast<registry_state_t *>(data)};
        registry_state.compositor = registry_state.compositor || std::strcmp(interface, wl_compositor_interface.name) == 0;
        registry_state.xdg_shell = registry_state.xdg_shell || std::strcmp(interface, xdg_wm_base_interface.name) == 0;
      },
      .global_remove = [](void *, wl_registry *, uint32_t) {},
    };
    wl_registry *const registry {wl_display_get_registry(display)};
    wl_registry_add_listener(registry, &listener, &state);
    const int roundtrip_result {wl_display_roundtrip(display)};
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (roundtrip_result < 0 || !state.compositor || !state.xdg_shell) {
      error = "Captured host Wayland compositor lacks wl_compositor or xdg_wm_base";
      return false;
    }
    return true;
#else
    error = "Wayland support is not compiled into this build";
    return false;
#endif
  }
}  // namespace host_desktop_endpoint
