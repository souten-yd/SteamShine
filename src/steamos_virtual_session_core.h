/**
 * @file src/steamos_virtual_session_core.h
 * @brief Pure SteamOS virtual-display request and Gamescope command helpers.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace steamos_virtual_session {
  /**
   * @brief Identifies who owns the selected Gamescope session.
   */
  enum class session_origin_e {
    none,  ///< No Gamescope source is selected.
    owned_private,  ///< SteamShine owns the Gamescope process and its private runtime.
    attached_existing,  ///< SteamShine only consumes a verified resident Gamescope source.
  };

  /**
   * @brief Return the stable status spelling for a Gamescope session origin.
   *
   * @param origin Ownership origin to serialize.
   * @return The lowercase status value.
   */
  std::string_view to_string(session_origin_e origin);

  /**
   * @brief Selects which class of Gamescope source a launch may use.
   */
  enum class session_source_policy_e {
    auto_select,  ///< Prefer a verified resident source, then an owned private source.
    existing_gamescope,  ///< Require a verified resident Gamescope source.
    owned_private,  ///< Require a SteamShine-owned private Gamescope source.
  };

  /**
   * @brief Parse a canonical SteamOS session-source policy value.
   *
   * @param value Configuration value to parse; matching is case-sensitive.
   * @return Parsed policy, or std::nullopt when the value is unsupported.
   */
  std::optional<session_source_policy_e> parse_session_source_policy(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a session-source policy.
   *
   * @param policy Policy to serialize.
   * @return The lowercase configuration value.
   */
  std::string_view to_string(session_source_policy_e policy);

  /**
   * @brief Selects whether an owned private session is mirrored locally.
   */
  enum class local_presentation_policy_e {
    auto_select,  ///< Mirror only when an owned session and host display are available.
    off,  ///< Keep the session remote-only.
    mirror,  ///< Require a local mirror for a SteamShine-owned private session.
  };

  /**
   * @brief Parse a canonical local-presentation policy value.
   *
   * @param value Configuration value to parse; matching is case-sensitive.
   * @return Parsed policy, or std::nullopt when unsupported.
   */
  std::optional<local_presentation_policy_e> parse_local_presentation_policy(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a local-presentation policy.
   *
   * @param policy Policy to serialize.
   * @return The lowercase configuration value.
   */
  std::string_view to_string(local_presentation_policy_e policy);

  /**
   * @brief Active presentation paths for the selected Gamescope session.
   */
  enum class presentation_e {
    remote_only,  ///< Stream only through the remote encoder path.
    remote_and_local,  ///< Stream remotely and mirror the owned private session locally.
  };

  /**
   * @brief Choose local presentation without mirroring an existing Game Mode output.
   *
   * @param policy Local presentation configuration.
   * @param origin Ownership origin of the selected Gamescope source.
   * @param host_wayland_available Whether a captured host Wayland endpoint exists.
   * @param physical_output_connected Whether a local physical output is available.
   * @return Desired presentation paths; unavailable local presentation fails closed to remote-only.
   */
  presentation_e decide_presentation(local_presentation_policy_e policy, session_origin_e origin, bool host_wayland_available, bool physical_output_connected);

  /**
   * @brief Return a stable status spelling for presentation paths.
   *
   * @param presentation Presentation paths to serialize.
   * @return The lowercase status value.
   */
  std::string_view to_string(presentation_e presentation);

  /**
   * @brief Policy governing whether SteamShine owns a virtual display.
   */
  enum class virtual_display_mode_e {
    off,  ///< Never create an owned virtual display.
    auto_detect,  ///< Create one only when no capturable output is available.
    force,  ///< Always create and capture from an owned headless Gamescope session.
  };

  /**
   * @brief Parse a canonical virtual-display mode value.
   *
   * @param value Configuration value to parse; matching is case-sensitive.
   * @return Parsed mode, or std::nullopt when the value is unsupported.
   */
  std::optional<virtual_display_mode_e> parse_virtual_display_mode(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a display mode.
   *
   * @param mode Mode to serialize.
   * @return The lowercase configuration value.
   */
  std::string_view to_string(virtual_display_mode_e mode);

  /**
   * @brief Capture route selected for one Moonlight application launch.
   */
  enum class session_route_e {
    physical_desktop,  ///< Capture the currently usable physical compositor output.
    attached_existing,  ///< Attach to a verified stock Game Mode Gamescope source.
    retained_owned_private,  ///< Reuse a compatible SteamShine-owned Gamescope source.
    new_owned_private,  ///< Create a new SteamShine-owned Gamescope source.
    reject,  ///< Fail closed because the configured route is unavailable.
  };

  /**
   * @brief Return the stable diagnostic spelling for a launch route.
   *
   * @param route Route to serialize.
   * @return Lowercase route name.
   */
  std::string_view to_string(session_route_e route);

  /**
   * @brief Inputs used to select one deterministic launch route.
   */
  struct session_route_input_t {
    bool feature_enabled;  ///< Whether the SteamOS virtual-display feature is enabled.
    virtual_display_mode_e mode;  ///< Requested virtual-display policy.
    session_source_policy_e source_policy;  ///< Configured Gamescope source policy.
    bool prefer_owned_session;  ///< Whether the selected application prefers an owned canvas over physical Desktop.
    bool prefer_physical_desktop;  ///< Whether a capture-only Desktop app prefers a usable physical compositor over resident Game Mode.
    bool startup_preflight_owned_session;  ///< Whether startup prepared an owned encoder-probe canvas that must survive the first launch decision.
    bool capturable_output_present;  ///< Whether a normal capture target is available.
    bool retained_owned_session;  ///< Whether SteamShine owns a compatible retained session.
    bool host_supported;  ///< Whether the host can create an owned Gamescope session.
    bool verified_existing_gamescope_present;  ///< Whether a verified resident Game Mode source is available.
  };

  /**
   * @brief Result of selecting a launch route.
   */
  struct session_route_decision_t {
    session_route_e route;  ///< Selected common or source-specific route.
    std::string reason;  ///< Stable diagnostic reason for the decision.
  };

  /**
   * @brief Select the capture route for one launch from current observations.
   *
   * Automatic policy normally prefers a verified stock Game Mode source. A
   * capture-only Desktop application instead selects a usable physical
   * compositor after the host has entered Desktop Mode. The first Big Picture
   * launch preserves a startup encoder-preflight canvas instead of racing a
   * transient stock Game Mode source. Other Big Picture launches still prefer
   * verified stock Game Mode, then compatible retained or new owned Gamescope
   * before physical Desktop. Explicit source policies remain authoritative.
   *
   * @param input Immutable policy and host-observation inputs.
   * @return Deterministic route and diagnostic reason.
   */
  session_route_decision_t select_session_route(const session_route_input_t &input);

  /**
   * @brief Report whether a selected route requires Gamescope capture handling.
   *
   * Rejected Gamescope policies remain on the fail-closed backend instead of
   * silently falling back to a physical capture source.
   *
   * @param route Selected launch route.
   * @return True for attached, retained, new, and rejected Gamescope routes.
   */
  bool route_uses_gamescope_capture(session_route_e route);

  /**
   * @brief Determine whether the physical Desktop has a usable capture path.
   *
   * A compositor can expose the connected output through KWin ScreenCast or
   * XDG Portal while sysfs reports its DRM CRTC disabled. Either an active
   * CRTC or a verified compositor source is sufficient, but a physical
   * connector is always required.
   *
   * @param physical_output_connected Whether a physical DRM connector is connected.
   * @param active_crtc_present Whether sysfs reports an enabled physical CRTC.
   * @param compositor_capture_available Whether a compositor exposed a capture source.
   * @return True when automatic policy can keep the physical Desktop.
   */
  bool physical_desktop_capturable(
    bool physical_output_connected,
    bool active_crtc_present,
    bool compositor_capture_available
  );

  /**
   * @brief Decide whether capture must use the private virtual compositor.
   *
   * @param virtual_display_required Whether policy initially requested a virtual display.
   * @param physical_output_connected Whether a physical DRM connector is connected.
   * @param compositor_capture_available Whether the host compositor exposed a capture source.
   * @param wlr_capture_forced Whether the user explicitly selected the WLR backend.
   * @return True when capture must remain on the private virtual compositor.
   */
  bool use_virtual_capture_backend(
    bool virtual_display_required,
    bool physical_output_connected,
    bool compositor_capture_available,
    bool wlr_capture_forced
  );

  /**
   * @brief Decide whether initialization should probe XDG Portal capture.
   *
   * Automatic capture probes Portal only for a connected physical output and
   * only when a higher-priority compositor backend was not verified. An
   * explicit Portal selection remains authoritative without a physical output.
   *
   * @param automatic_capture Whether no capture backend was explicitly selected.
   * @param portal_explicitly_requested Whether the user selected the Portal backend.
   * @param physical_output_connected Whether a physical DRM connector is connected.
   * @param higher_priority_available Whether KWin or another preferred backend is ready.
   * @return True when Portal discovery should run.
   */
  bool should_probe_physical_portal(
    bool automatic_capture,
    bool portal_explicitly_requested,
    bool physical_output_connected,
    bool higher_priority_available
  );

  /**
   * @brief Derive the EIS socket path advertised by a Gamescope Wayland display.
   *
   * @param runtime_directory Trusted absolute runtime directory.
   * @param gamescope_wayland_display Verified single-component Gamescope display name.
   * @return Contained EIS socket path, or no value for an unsafe input.
   */
  std::optional<std::filesystem::path> gamescope_eis_socket_path(
    const std::filesystem::path &runtime_directory,
    std::string_view gamescope_wayland_display
  );

  /**
   * @brief Policy applied when a requested coded extent needs alignment.
   */
  enum class geometry_alignment_policy_e {
    auto_align,  ///< Minimally align a representable request and report the change.
    require_exact,  ///< Reject a request that cannot be represented exactly.
  };

  /**
   * @brief Parse a geometry alignment policy.
   *
   * @param value Configuration value; matching is case-sensitive.
   * @return Parsed policy, or no value for unsupported text.
   */
  std::optional<geometry_alignment_policy_e> parse_geometry_alignment_policy(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for an alignment policy.
   *
   * @param policy Policy to serialize.
   * @return Lowercase configuration value.
   */
  std::string_view to_string(geometry_alignment_policy_e policy);

  /**
   * @brief Policy for absolute input that lands in fitted-output margins.
   */
  enum class margin_input_policy_e {
    clamp,  ///< Clamp margin input to the closest visible content edge.
    reject,  ///< Drop margin input instead of moving at the content edge.
  };

  /**
   * @brief Parse an absolute-input margin policy.
   *
   * @param value Configuration value; matching is case-sensitive.
   * @return Parsed policy, or no value for unsupported text.
   */
  std::optional<margin_input_policy_e> parse_margin_input_policy(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a margin policy.
   *
   * @param policy Policy to serialize.
   * @return Lowercase configuration value.
   */
  std::string_view to_string(margin_input_policy_e policy);

  /**
   * @brief Exact display refresh represented as a reduced rational number.
   */
  struct display_refresh_t {
    std::uint32_t numerator {0};  ///< Refresh cycles in one denominator interval.
    std::uint32_t denominator {1};  ///< Positive interval denominator.
  };

  /**
   * @brief Safety and administrator ceilings for one display request.
   */
  struct display_constraints_t {
    int minimum_width {640};  ///< Smallest accepted coded width.
    int maximum_width {7680};  ///< Largest accepted coded width.
    int minimum_height {480};  ///< Smallest accepted coded height.
    int maximum_height {4320};  ///< Largest accepted coded height.
    std::uint32_t width_alignment {2};  ///< Required coded-width alignment.
    std::uint32_t height_alignment {2};  ///< Required coded-height alignment.
    std::uint32_t minimum_fps {30};  ///< Smallest accepted refresh rate.
    std::uint32_t maximum_fps {240};  ///< Administrator refresh ceiling.
    std::uint64_t maximum_frame_pixels {33177600};  ///< Encoder coded-extent ceiling.
    std::uint64_t maximum_pixel_rate {1990656000};  ///< Probed/administrator pixels-per-second ceiling.
    std::uint64_t maximum_buffer_bytes {536870912};  ///< GPU/capture buffer budget for the stream.
    std::uint32_t buffer_count {4};  ///< Conservatively budgeted simultaneous frame buffers.
    std::uint32_t bytes_per_pixel {4};  ///< Worst-case bytes per pixel used for budget checks.
  };

  /**
   * @brief A validated nested Gamescope display request and its preserved input.
   */
  struct display_request_t {
    int requested_width {0};  ///< Original client width before alignment.
    int requested_height {0};  ///< Original client height before alignment.
    display_refresh_t requested_refresh;  ///< Original exact refresh request.
    int width {0};  ///< Selected coded width in pixels.
    int height {0};  ///< Selected coded height in pixels.
    int fps {0};  ///< Rounded legacy refresh used by integer-only consumers.
    display_refresh_t refresh;  ///< Selected exact refresh.
    bool valid {false};  ///< Whether every safety and capability check passed.
    bool adjusted {false};  ///< Whether missing or unaligned input changed selection.
    std::string reason;  ///< Stable selection, adjustment, or rejection reason.
  };

  /**
   * @brief Select a safe coded extent and rational refresh for one request.
   *
   * @param requested_width Client-provided width, or zero with all request fields when unavailable.
   * @param requested_height Client-provided height, or zero with all request fields when unavailable.
   * @param requested_fps Client integer FPS.
   * @param requested_refresh_x100 Optional exact refresh in hundredths of Hz.
   * @param default_width Configured width used only for a wholly missing request.
   * @param default_height Configured height used only for a wholly missing request.
   * @param default_fps Configured FPS used only for a wholly missing request.
   * @param alignment_policy Whether unaligned dimensions may be minimally adjusted.
   * @param constraints Effective probed and administrator ceilings.
   * @return Selected request or a stable rejected result.
   */
  display_request_t select_display_request(
    int requested_width,
    int requested_height,
    int requested_fps,
    int requested_refresh_x100,
    int default_width,
    int default_height,
    int default_fps,
    geometry_alignment_policy_e alignment_policy,
    const display_constraints_t &constraints = {}
  );

  /**
   * @brief Clamp a client display request to SteamOS virtual-session bounds.
   *
   * @param requested_width Client-provided width, or zero when unavailable.
   * @param requested_height Client-provided height, or zero when unavailable.
   * @param requested_fps Client-provided frame rate, or zero when unavailable.
   * @param default_width Configured width used for a missing request.
   * @param default_height Configured height used for a missing request.
   * @param default_fps Configured frame rate used for a missing request.
   * @return A request constrained to 640x480 through 7680x4320 at 30 through 240 FPS.
   */
  display_request_t normalize_display_request(int requested_width, int requested_height, int requested_fps, int default_width, int default_height, int default_fps);

  /**
   * @brief Immutable facts that determine whether an owned canvas can be reused.
   */
  struct retained_session_key_t {
    int width {0};  ///< Owned canvas width.
    int height {0};  ///< Owned canvas height.
    display_refresh_t refresh;  ///< Owned canvas refresh.
    bool hdr {false};  ///< HDR intent used when the canvas was created.
    std::string render_node;  ///< GPU render node shared by game, capture, and encoder.
    std::string source_identity;  ///< Stable owned source identity.
    std::string capture_pixel_format;  ///< Required capture format class such as NV12 or P010.
  };

  /**
   * @brief Compare every retained-session compatibility dimension.
   *
   * @param retained Facts recorded for the existing owned session.
   * @param requested Facts required by the new launch.
   * @return True only when reuse cannot change canvas, color, GPU, source, or capture format.
   */
  bool retained_session_compatible(const retained_session_key_t &retained, const retained_session_key_t &requested);

  /**
   * @brief Visible source content fitted into one encoded output.
   */
  struct content_rectangle_t {
    int x {0};  ///< Left output offset in pixels.
    int y {0};  ///< Top output offset in pixels.
    int width {0};  ///< Visible content width in output pixels.
    int height {0};  ///< Visible content height in output pixels.
  };

  /**
   * @brief Compute a centered aspect-preserving fit rectangle.
   *
   * @param source_width Source canvas width.
   * @param source_height Source canvas height.
   * @param output_width Encoded output width.
   * @param output_height Encoded output height.
   * @param alignment Output rectangle alignment.
   * @return Centered rectangle, or an empty rectangle for invalid dimensions.
   */
  content_rectangle_t fit_content_rectangle(int source_width, int source_height, int output_width, int output_height, int alignment = 2);

  /**
   * @brief Result of mapping one encoded-output coordinate into visible content.
   */
  struct content_coordinate_t {
    bool accepted {false};  ///< Whether the configured margin policy accepts the coordinate.
    double x {0.0};  ///< Normalized horizontal source coordinate in the range zero through one.
    double y {0.0};  ///< Normalized vertical source coordinate in the range zero through one.
  };

  /**
   * @brief Map an encoded-output coordinate through its fitted content rectangle.
   *
   * @param output_x Horizontal output coordinate.
   * @param output_y Vertical output coordinate.
   * @param rectangle Visible fitted content rectangle.
   * @param policy Margin clamp or rejection policy.
   * @return Normalized source coordinate and acceptance state.
   */
  content_coordinate_t map_content_coordinate(double output_x, double output_y, const content_rectangle_t &rectangle, margin_input_policy_e policy);

  /**
   * @brief Build a Gamescope command from options advertised by its own help text.
   *
   * The result never uses an option absent from @p help_text. This keeps the
   * virtual-display provider compatible with the Gamescope version installed on
   * the SteamOS host. Owned sessions intentionally omit `--steam` so ordinary
   * applications without a Steam AppID remain eligible for focus and capture.
   *
   * @param help_text Output captured from `gamescope --help`.
   * Modern headless Gamescope receives both nested game dimensions and output
   * canvas dimensions. Omitting the latter silently selects its 1280x720
   * default and forces an unintended encode upscale.
   *
   * @param width Normalized nested and output-canvas width.
   * @param height Normalized nested and output-canvas height.
   * @param fps Normalized nested display refresh rate.
   * @param enable_hdr Whether the client requested HDR output.
   * @param gpu_device PCI vendor/device identifier accepted by Gamescope, if selected.
   * @param error Receives a reason when the advertised option set is insufficient.
   * @return Arguments after the executable, or an empty vector on failure.
   */
  std::vector<std::string> gamescope_arguments(const std::string &help_text, int width, int height, int fps, bool enable_hdr, const std::string &gpu_device, std::string &error);
}  // namespace steamos_virtual_session
