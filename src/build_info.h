/**
 * @file src/build_info.h
 * @brief Stable accessors for build-specific version metadata.
 */
#pragma once

#include <string_view>

namespace build_info {
  /**
   * @brief Return the configured application version.
   *
   * @return Version string embedded in this binary.
   */
  std::string_view version();

  /**
   * @brief Return the source commit embedded in this binary.
   *
   * @return Full or abbreviated source commit supplied by the build.
   */
  std::string_view commit();
}  // namespace build_info
