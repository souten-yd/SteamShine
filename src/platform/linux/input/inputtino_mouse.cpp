/**
 * @file src/platform/linux/input/inputtino_mouse.cpp
 * @brief Definitions for inputtino mouse input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_mouse.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::mouse {

  /**
   * @brief Apply a relative pointer movement to the virtual mouse.
   */
  void move(input_raw_t *raw, int deltaX, int deltaY) {
    if (!gamescope_input_uses_desktop_device(raw->gamescope_eis.move(deltaX, deltaY))) {
      return;
    }
    if (auto *mouse = raw->desktop_mouse()) {
      mouse->move(deltaX, deltaY);
    }
  }

  /**
   * @brief Move abs using the backend coordinate system.
   */
  void move_abs(input_raw_t *raw, const touch_port_t &touch_port, float x, float y) {
    if (!gamescope_input_uses_desktop_device(raw->gamescope_eis.move_absolute(x, y))) {
      return;
    }
    if (auto *mouse = raw->desktop_mouse()) {
      mouse->move_abs(x, y, touch_port.width, touch_port.height);
    }
  }

  /**
   * @brief Press or release a virtual mouse button.
   */
  void button(input_raw_t *raw, int button, bool release) {
    std::uint32_t linux_button {};
    switch (button) {
      case BUTTON_LEFT:
        linux_button = BTN_LEFT;
        break;
      case BUTTON_MIDDLE:
        linux_button = BTN_MIDDLE;
        break;
      case BUTTON_RIGHT:
        linux_button = BTN_RIGHT;
        break;
      case BUTTON_X1:
        linux_button = BTN_SIDE;
        break;
      case BUTTON_X2:
        linux_button = BTN_EXTRA;
        break;
      default:
        BOOST_LOG(warning) << "Unknown mouse button: " << button;
        return;
    }
    if (!gamescope_input_uses_desktop_device(raw->gamescope_eis.button(linux_button, !release))) {
      return;
    }
    if (auto *mouse = raw->desktop_mouse()) {
      inputtino::Mouse::MOUSE_BUTTON btn_type;
      switch (button) {
        case BUTTON_LEFT:
          btn_type = inputtino::Mouse::LEFT;
          break;
        case BUTTON_MIDDLE:
          btn_type = inputtino::Mouse::MIDDLE;
          break;
        case BUTTON_RIGHT:
          btn_type = inputtino::Mouse::RIGHT;
          break;
        case BUTTON_X1:
          btn_type = inputtino::Mouse::SIDE;
          break;
        case BUTTON_X2:
          btn_type = inputtino::Mouse::EXTRA;
          break;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: " << button;
          return;
      }
      if (release) {
        mouse->release(btn_type);
      } else {
        mouse->press(btn_type);
      }
    }
  }

  /**
   * @brief Apply a vertical scroll event to the virtual mouse.
   */
  void scroll(input_raw_t *raw, int high_res_distance) {
    if (!gamescope_input_uses_desktop_device(raw->gamescope_eis.scroll(0, high_res_distance))) {
      return;
    }
    if (auto *mouse = raw->desktop_mouse()) {
      mouse->vertical_scroll(high_res_distance);
    }
  }

  /**
   * @brief Apply a horizontal scroll event to the virtual mouse.
   */
  void hscroll(input_raw_t *raw, int high_res_distance) {
    if (!gamescope_input_uses_desktop_device(raw->gamescope_eis.scroll(high_res_distance, 0))) {
      return;
    }
    if (auto *mouse = raw->desktop_mouse()) {
      mouse->horizontal_scroll(high_res_distance);
    }
  }

  /**
   * @brief Return the current virtual pointer location.
   */
  util::point_t get_location(input_raw_t *raw) {
    // Do not instantiate the desktop-only uinput mouse for a location query.
    // TODO: decide what to do after https://github.com/games-on-whales/inputtino/issues/6 is resolved.
    static_cast<void>(raw);
    return {0, 0};
  }
}  // namespace platf::mouse
