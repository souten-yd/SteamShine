/**
 * @file src/platform/linux/input/input_key_mapping.h
 * @brief Pure Linux key translation helpers.
 */
#pragma once

#include <cstdint>
#include <optional>

namespace platf::keyboard {
  /**
   * @brief Translate a Moonlight/Windows virtual key to a Linux key code.
   *
   * @param modcode Moonlight virtual-key value.
   * @return Linux `KEY_*` value, or no value for an unsupported key.
   */
  std::optional<std::uint32_t> linux_keycode(std::uint16_t modcode);
}  // namespace platf::keyboard
