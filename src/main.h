/**
 * @file src/main.h
 * @brief Declarations for the main entry point for Sunshine.
 */
#pragma once

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
 * @brief Main application entry point.
 * @examples
 * main(1, const char* args[] = {"sunshine", nullptr});
 * @examples_end
 *
 * @return Process or platform callback exit code.
 */
int main(int argc, char *argv[]);
