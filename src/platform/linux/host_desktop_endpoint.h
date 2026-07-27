/**
 * @file src/platform/linux/host_desktop_endpoint.h
 * @brief Refreshable host desktop endpoint retained for local presentation.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace host_desktop_endpoint {
  /**
   * @brief Host desktop connection values captured before private-session setup.
   */
  struct endpoint_t {
    std::string xdg_runtime_directory;  ///< Original host `XDG_RUNTIME_DIR`.
    std::string wayland_display;  ///< Original host `WAYLAND_DISPLAY`.
    std::string x11_display;  ///< Original host `DISPLAY`.
    std::uint64_t generation {};  ///< Monotonically increasing accepted endpoint generation.
  };

  /**
   * @brief Capture a complete current process desktop endpoint when available.
   *
   * Empty or partial environments never replace a complete endpoint. This
   * permits a user service that began before the desktop to acquire its first
   * valid endpoint later. SteamShine never modifies its own environment for
   * private Gamescope children, so those child values cannot overwrite this
   * process-owned endpoint.
   */
  void capture();

  /**
   * @brief Decide whether a candidate may replace the current endpoint.
   *
   * @param current Endpoint currently retained by SteamShine.
   * @param candidate Newly observed desktop endpoint.
   * @return True when the candidate is complete and materially different.
   */
  bool should_refresh(const endpoint_t &current, const endpoint_t &candidate);

  /**
   * @brief Return the latest accepted host desktop endpoint.
   *
   * @return Values captured by @ref capture, or empty fields before capture.
   */
  endpoint_t current();

  /**
   * @brief Verify that the captured host endpoint supports native Wayland presentation.
   *
   * The probe validates the runtime directory and socket type/owner on every
   * call, then connects directly without modifying the process environment
   * used by private Gamescope children.
   *
   * @param error Human-readable reason when host presentation is unavailable.
   * @return True only when both `wl_compositor` and `xdg_wm_base` are advertised.
   */
  bool supports_wayland_presentation(std::string &error);

  /**
   * @brief A host-owned xdg-shell surface reserved for local Vulkan presentation.
   *
   * The window owns a distinct connection to the captured host compositor. It
   * never uses a private Gamescope display or changes process environment.
   */
  class wayland_presentation_window_t {
  public:
    /**
     * @brief Construct an inactive host presentation window.
     */
    wayland_presentation_window_t();

    /**
     * @brief Destroy the xdg-shell surface and its host connection.
     */
    ~wayland_presentation_window_t();

    wayland_presentation_window_t(const wayland_presentation_window_t &) = delete;
    wayland_presentation_window_t &operator=(const wayland_presentation_window_t &) = delete;

    /**
     * @brief Create and configure one host-compositor xdg-shell surface.
     *
     * @param error Human-readable failure reason.
     * @return True after the compositor has configured the surface.
     */
    bool start(std::string &error);

    /**
     * @brief Tear down the surface without affecting Gamescope or PipeWire.
     */
    void stop();

    /**
     * @brief Return the native `wl_display` pointer for Vulkan surface creation.
     *
     * @return Opaque native display pointer, or null when inactive.
     */
    void *display_handle() const;

    /**
     * @brief Return the native `wl_surface` pointer for Vulkan surface creation.
     *
     * @return Opaque native surface pointer, or null when inactive.
     */
    void *surface_handle() const;

  private:
    struct impl_t;  ///< Wayland-private state defined in the implementation.
    std::unique_ptr<impl_t> impl_;  ///< Owned native presentation state.
  };
}  // namespace host_desktop_endpoint
