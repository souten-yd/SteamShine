/**
 * @file src/steamshine_x52.h
 * @brief Connected X52 HOTAS detection, profiles, and supported display controls.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <stop_token>
#include <string>

namespace steamshine_x52 {
  /**
   * @brief Return a validated profile with the recommended X52 defaults.
   *
   * @return Complete profile JSON.
   */
  nlohmann::json recommended_profile();

  /**
   * @brief Validate and normalize one standard X52 profile.
   *
   * @param input Profile supplied by the Web UI or persistent configuration.
   * @param error Validation error when the profile is rejected.
   * @return Complete profile, or a discarded JSON value on failure.
   */
  nlohmann::json validate_profile(const nlohmann::json &input, std::string &error);

  /**
   * @brief Return the currently connected X52 and saved profiles.
   *
   * @return Addon state with hardware capability information.
   */
  nlohmann::json status();

  /**
   * @brief Save a profile, addon state, or physical input preference while an X52 is connected.
   *
   * @param input Profile and action supplied by the authenticated Web UI.
   * @param error Failure reason.
   * @return True when the requested change was saved and applied.
   */
  bool save(const nlohmann::json &input, std::string &error);

  /**
   * @brief Restore the active profile and physical input preference after startup or reconnection.
   *
   * @param stop Cancellation requested when Sunshine exits.
   */
  void monitor(std::stop_token stop);
}  // namespace steamshine_x52
