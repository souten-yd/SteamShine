/**
 * @file src/platform/linux/host_desktop_endpoint.cpp
 * @brief Refreshable host desktop endpoint implementation.
 */
#include "host_desktop_endpoint.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(SUNSHINE_BUILD_WAYLAND)
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <wayland-client.h>
  #include <xdg-shell.h>
#endif

namespace host_desktop_endpoint {
  namespace {
    std::mutex endpoint_mutex;  ///< Serializes endpoint refreshes.
    endpoint_t endpoint;  ///< Latest complete host endpoint.

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

#if defined(SUNSHINE_BUILD_WAYLAND)
    /**
     * @brief Validate and construct the current user's host Wayland socket path.
     *
     * @param host Candidate host endpoint.
     * @param socket_path Receives the validated socket path.
     * @param error Receives a human-readable rejection reason.
     * @return True when the runtime and socket are current-user-owned and safe.
     */
    bool host_socket_path(const endpoint_t &host, std::string &socket_path, std::string &error) {
      if (host.xdg_runtime_directory.empty() || host.wayland_display.empty() || host.wayland_display.find('/') != std::string::npos) {
        error = "Captured host Wayland endpoint is unavailable";
        return false;
      }
      socket_path = host.xdg_runtime_directory + "/" + host.wayland_display;
      if (socket_path.size() >= sizeof(sockaddr_un {}.sun_path)) {
        error = "Captured host Wayland socket path is too long";
        return false;
      }
      struct stat runtime_status {};
      struct stat socket_status {};
      if (::lstat(host.xdg_runtime_directory.c_str(), &runtime_status) != 0 || !S_ISDIR(runtime_status.st_mode) || runtime_status.st_uid != ::geteuid()) {
        error = "Captured host Wayland runtime is not a current-user-owned directory";
        return false;
      }
      if (::lstat(socket_path.c_str(), &socket_status) != 0 || !S_ISSOCK(socket_status.st_mode) || socket_status.st_uid != ::geteuid()) {
        error = "Captured host Wayland endpoint is not a current-user-owned UNIX socket";
        return false;
      }
      return true;
    }
#endif
  }  // namespace

  bool should_refresh(const endpoint_t &current, const endpoint_t &candidate) {
    const bool candidate_complete {!candidate.xdg_runtime_directory.empty() && !candidate.wayland_display.empty()};
    return candidate_complete &&
           (current.xdg_runtime_directory != candidate.xdg_runtime_directory ||
            current.wayland_display != candidate.wayland_display ||
            current.x11_display != candidate.x11_display);
  }

  void capture() {
    const endpoint_t candidate {
      .xdg_runtime_directory = environment_value("XDG_RUNTIME_DIR"),
      .wayland_display = environment_value("WAYLAND_DISPLAY"),
      .x11_display = environment_value("DISPLAY"),
    };
    std::scoped_lock lock {endpoint_mutex};
    if (should_refresh(endpoint, candidate)) {
      endpoint = candidate;
      ++endpoint.generation;
    }
  }

  endpoint_t current() {
    std::scoped_lock lock {endpoint_mutex};
    return endpoint;
  }

  bool supports_wayland_presentation(std::string &error) {
#if defined(SUNSHINE_BUILD_WAYLAND)
    const auto host {current()};
    std::string socket_path;
    if (!host_socket_path(host, socket_path, error)) {
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
      .global_remove = [](void *, wl_registry *, uint32_t) {
      },
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

  /**
   * @brief Private host Wayland presentation objects.
   */
  struct wayland_presentation_window_t::impl_t {
#if defined(SUNSHINE_BUILD_WAYLAND)
    wl_display *display {nullptr};  ///< Dedicated host compositor connection.
    wl_compositor *compositor {nullptr};  ///< Host compositor interface.
    xdg_wm_base *shell {nullptr};  ///< Host xdg-shell interface.
    wl_surface *surface {nullptr};  ///< Native surface consumed by Vulkan.
    xdg_surface *shell_surface {nullptr};  ///< xdg-shell surface role.
    xdg_toplevel *toplevel {nullptr};  ///< Top-level window role.
    bool configured {false};  ///< Whether the compositor supplied an initial configure.
#endif
  };

  wayland_presentation_window_t::wayland_presentation_window_t():
      impl_ {std::make_unique<impl_t>()} {}

  wayland_presentation_window_t::~wayland_presentation_window_t() {
    stop();
  }

  bool wayland_presentation_window_t::start(std::string &error) {
    stop();
#if defined(SUNSHINE_BUILD_WAYLAND)
    const auto host {current()};
    std::string socket_path;
    if (!host_socket_path(host, socket_path, error)) {
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
    impl_->display = wl_display_connect_to_fd(fd);
    if (!impl_->display) {
      ::close(fd);
      error = "Failed to create a host Wayland client connection";
      return false;
    }

    struct registry_state_t {
      wayland_presentation_window_t::impl_t *window;  ///< Window receiving bound globals.
    } state {impl_.get()};

    const wl_registry_listener registry_listener {
      .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        auto &window {*static_cast<registry_state_t *>(data)->window};
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
          window.compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
        } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
          window.shell = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 1U)));
        }
      },
      .global_remove = [](void *, wl_registry *, uint32_t) {
      },
    };
    wl_registry *const registry {wl_display_get_registry(impl_->display)};
    wl_registry_add_listener(registry, &registry_listener, &state);
    if (wl_display_roundtrip(impl_->display) < 0 || !impl_->compositor || !impl_->shell) {
      wl_registry_destroy(registry);
      stop();
      error = "Captured host Wayland compositor lacks wl_compositor or xdg_wm_base";
      return false;
    }
    const xdg_wm_base_listener wm_base_listener {
      .ping = [](void *, xdg_wm_base *wm_base, uint32_t serial) {
        xdg_wm_base_pong(wm_base, serial);
      },
    };
    xdg_wm_base_add_listener(impl_->shell, &wm_base_listener, nullptr);
    impl_->surface = wl_compositor_create_surface(impl_->compositor);
    impl_->shell_surface = xdg_wm_base_get_xdg_surface(impl_->shell, impl_->surface);
    const xdg_surface_listener surface_listener {
      .configure = [](void *data, xdg_surface *surface, uint32_t serial) {
        auto &window {*static_cast<impl_t *>(data)};
        xdg_surface_ack_configure(surface, serial);
        window.configured = true;
      },
    };
    xdg_surface_add_listener(impl_->shell_surface, &surface_listener, impl_.get());
    impl_->toplevel = xdg_surface_get_toplevel(impl_->shell_surface);
    xdg_toplevel_set_title(impl_->toplevel, "SteamShine Local Presentation");
    wl_surface_commit(impl_->surface);
    if (wl_display_roundtrip(impl_->display) < 0 || !impl_->configured) {
      wl_registry_destroy(registry);
      stop();
      error = "Host Wayland compositor did not configure the presentation surface";
      return false;
    }
    wl_registry_destroy(registry);
    return true;
#else
    error = "Wayland support is not compiled into this build";
    return false;
#endif
  }

  void wayland_presentation_window_t::stop() {
#if defined(SUNSHINE_BUILD_WAYLAND)
    if (impl_->toplevel) {
      xdg_toplevel_destroy(impl_->toplevel);
      impl_->toplevel = nullptr;
    }
    if (impl_->shell_surface) {
      xdg_surface_destroy(impl_->shell_surface);
      impl_->shell_surface = nullptr;
    }
    if (impl_->surface) {
      wl_surface_destroy(impl_->surface);
      impl_->surface = nullptr;
    }
    if (impl_->shell) {
      xdg_wm_base_destroy(impl_->shell);
      impl_->shell = nullptr;
    }
    if (impl_->compositor) {
      wl_compositor_destroy(impl_->compositor);
      impl_->compositor = nullptr;
    }
    if (impl_->display) {
      wl_display_disconnect(impl_->display);
      impl_->display = nullptr;
    }
    impl_->configured = false;
#endif
  }

  void *wayland_presentation_window_t::display_handle() const {
#if defined(SUNSHINE_BUILD_WAYLAND)
    return impl_->display;
#else
    return nullptr;
#endif
  }

  void *wayland_presentation_window_t::surface_handle() const {
#if defined(SUNSHINE_BUILD_WAYLAND)
    return impl_->surface;
#else
    return nullptr;
#endif
  }
}  // namespace host_desktop_endpoint
