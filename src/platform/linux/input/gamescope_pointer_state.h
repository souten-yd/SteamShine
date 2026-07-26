/**
 * @file src/platform/linux/input/gamescope_pointer_state.h
 * @brief Shared EIS pointer-button state for touch and pen emulation.
 */
#pragma once

#include <optional>

namespace platf {
  /**
   * @brief Identifies a pointer-like source sharing the EIS primary button.
   */
  enum class gamescope_pointer_source_e {
    touch,  ///< Moonlight touchscreen input.
    pen,  ///< Moonlight pen input.
  };

  /**
   * @brief Track touch and pen holds as one EIS primary-button state.
   *
   * Touch and pen are converted to a pointer for Gamescope because EIS does not
   * expose the virtual touch devices used by the desktop backend. A transition
   * is emitted only when the combined hold state changes, preventing one source
   * from releasing a button still held by the other.
   */
  class gamescope_pointer_state_t {
  public:
    /**
     * @brief Update one source and return a combined button transition.
     *
     * @param source Pointer-like input source being updated.
     * @param pressed Whether that source currently holds the primary button.
     * @return New combined pressed state, or no value when no EIS event is needed.
     */
    std::optional<bool> update(gamescope_pointer_source_e source, bool pressed) {
      const bool was_pressed {touch_pressed_ || pen_pressed_};
      if (source == gamescope_pointer_source_e::touch) {
        touch_pressed_ = pressed;
      } else {
        pen_pressed_ = pressed;
      }
      const bool is_pressed {touch_pressed_ || pen_pressed_};
      if (was_pressed == is_pressed) {
        return std::nullopt;
      }
      return is_pressed;
    }

    /**
     * @brief Clear all sources and report whether a release must be emitted.
     *
     * @return True when the combined button was pressed before the reset.
     */
    bool reset() {
      const bool was_pressed {touch_pressed_ || pen_pressed_};
      touch_pressed_ = false;
      pen_pressed_ = false;
      return was_pressed;
    }

  private:
    bool touch_pressed_ {};  ///< Whether touch currently holds the primary button.
    bool pen_pressed_ {};  ///< Whether pen currently holds the primary button.
  };
}  // namespace platf
