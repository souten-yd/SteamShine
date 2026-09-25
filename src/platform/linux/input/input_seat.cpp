/**
 * @file src/platform/linux/input/input_seat.cpp
 * @brief Implementation for multi-seat naming (udev-only).
 */
// lib includes
#include <lizardbyte/common/env.h>

// local includes
#include "input_seat.h"

namespace platf::input_seat {

  std::string get_target_seat() {
    if (std::string seat; lizardbyte::common::get_env("XDG_SEAT", seat) && !seat.empty()) {
      return seat;
    }

    return {};
  }

}  // namespace platf::input_seat
