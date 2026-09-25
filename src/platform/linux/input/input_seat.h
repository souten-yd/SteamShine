/**
 * @file src/platform/linux/input/input_seat.h
 * @brief Helpers for multi-seat naming (udev-only).
 */
#pragma once

#include <string>

namespace platf::input_seat {

  /**
   * Determine the target seat for the current Sunshine instance.
   * Returns empty string if no seat could be determined.
   *
   * @return Seat name used for virtual input devices, or an empty string when unknown.
   */
  std::string get_target_seat();

}  // namespace platf::input_seat
