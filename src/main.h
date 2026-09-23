/**
 * @file src/main.h
 * @brief Declarations for the main entry point for Sunshine.
 */
#pragma once

#include <chrono>

/**
 * @brief Decide whether an ended platform event loop must keep the server alive.
 *
 * Desktop event loops, such as the Linux system tray, can end when their
 * compositor exits during a Desktop Mode to Game Mode transition. Only an
 * explicit Sunshine shutdown request may allow that event to terminate the
 * resident server.
 *
 * @param shutdown_requested Whether Sunshine has received a shutdown request.
 * @return True when the server must continue waiting for a shutdown request.
 */
constexpr bool main_loop_requires_shutdown_wait(const bool shutdown_requested) noexcept {
  return !shutdown_requested;
}

/**
 * @brief Decide whether the optional system tray may run in the resident server.
 *
 * SteamOS virtual-display mode supports transitions between desktop and Game
 * Mode compositors. Qt platform backends can terminate the process when their
 * compositor disappears, so transition-capable servers must omit the tray.
 *
 * @param tray_requested Whether tray support is compiled, configured, and requested.
 * @param steamos_transitions_enabled Whether SteamOS virtual-display transitions are enabled.
 * @return True when the tray may safely run inside the resident server.
 */
constexpr bool main_loop_uses_system_tray(const bool tray_requested, const bool steamos_transitions_enabled) noexcept {
  return tray_requested && !steamos_transitions_enabled;
}

/**
 * @brief Calculate the force-shutdown watchdog for orderly session recovery.
 *
 * A SteamOS shutdown may consume both the configured owned-session stop
 * timeout and the stock-session startup timeout. The additional margin covers
 * application undo commands and server-thread joins. Non-transitioning hosts
 * retain the historical ten-second watchdog.
 *
 * @param steamos_transitions_enabled Whether virtual-display transitions are enabled.
 * @param virtual_shutdown_seconds Maximum owned-session stop time.
 * @param stock_startup_seconds Maximum stock-session restoration time.
 * @return Bounded time allowed before forced process termination.
 */
constexpr std::chrono::seconds shutdown_watchdog_timeout(
  const bool steamos_transitions_enabled,
  const int virtual_shutdown_seconds,
  const int stock_startup_seconds
) noexcept {
  constexpr int baseline_seconds {10};
  constexpr int cleanup_margin_seconds {10};
  if (!steamos_transitions_enabled) {
    return std::chrono::seconds {baseline_seconds};
  }
  const int safe_virtual_shutdown {virtual_shutdown_seconds > 0 ? virtual_shutdown_seconds : 0};
  const int safe_stock_startup {stock_startup_seconds > 0 ? stock_startup_seconds : 0};
  const int orderly_shutdown_seconds {safe_virtual_shutdown + safe_stock_startup + cleanup_margin_seconds};
  return std::chrono::seconds {orderly_shutdown_seconds > baseline_seconds ? orderly_shutdown_seconds : baseline_seconds};
}

/**
 * @brief Decide whether failed physical capture initialization requires Gamescope preflight.
 *
 * Active DRM scanout alone does not prove that a resident user service may
 * capture it. When platform initialization has already failed, an enabled
 * SteamOS virtual-display mode must use the verified Gamescope path instead.
 *
 * @param platform_initialized Whether the normal platform capture layer initialized.
 * @param virtual_display_enabled Whether SteamOS virtual-display support is enabled.
 * @param virtual_mode_enabled Whether the configured mode permits virtual capture.
 * @return True when startup must force a Gamescope encoder preflight.
 */
constexpr bool should_force_virtual_encoder_preflight(
  const bool platform_initialized,
  const bool virtual_display_enabled,
  const bool virtual_mode_enabled
) noexcept {
  return !platform_initialized && virtual_display_enabled && virtual_mode_enabled;
}

/**
 * @brief Decide whether startup must prepare a Gamescope encoder endpoint.
 *
 * @param capture_backend_required Whether an owned or attached endpoint is already active.
 * @param transitions_enabled Whether SteamOS virtual-display transitions are enabled.
 * @param virtual_mode_enabled Whether the Display-tab mode permits Gamescope routing.
 * @return True when encoder probing must run against a verified Gamescope endpoint.
 */
constexpr bool should_prepare_virtual_encoder_preflight(
  const bool capture_backend_required,
  const bool transitions_enabled,
  const bool virtual_mode_enabled
) noexcept {
  return capture_backend_required || (transitions_enabled && virtual_mode_enabled);
}

/**
 * @brief Main application entry point.
 * @examples
 * main(1, const char* args[] = {"sunshine", nullptr});
 * @examples_end
 *
 * @return Process or platform callback exit code.
 */
int main(int argc, char *argv[]);
