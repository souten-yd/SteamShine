/**
 * @file src/platform/linux/input/inputtino_touch.cpp
 * @brief Definitions for inputtino touch input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_touch.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::touch {
  /**
   * @brief Apply the supplied state update to the platform backend.
   */
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const touch_input_t &touch) {
    const auto route {raw->global->gamescope_eis.refresh()};
    if (route != gamescope_input_result_e::desktop) {
      if (route == gamescope_input_result_e::blocked) {
        raw->gamescope_pointer_state.reset();
      }
      if (route == gamescope_input_result_e::delivered) {
        switch (touch.eventType) {
          case LI_TOUCH_EVENT_HOVER:
          case LI_TOUCH_EVENT_DOWN:
          case LI_TOUCH_EVENT_MOVE:
            raw->global->gamescope_eis.move_absolute(touch.x, touch.y);
            if (touch.eventType == LI_TOUCH_EVENT_DOWN) {
              if (const auto transition {raw->gamescope_pointer_state.update(gamescope_pointer_source_e::touch, true)}) {
                raw->global->gamescope_eis.button(BTN_LEFT, *transition);
              }
            }
            break;
          case LI_TOUCH_EVENT_CANCEL:
          case LI_TOUCH_EVENT_UP:
          case LI_TOUCH_EVENT_HOVER_LEAVE:
            if (const auto transition {raw->gamescope_pointer_state.update(gamescope_pointer_source_e::touch, false)}) {
              raw->global->gamescope_eis.button(BTN_LEFT, *transition);
            }
            break;
        }
      }
      return;
    }
    if (raw->touch) {
      switch (touch.eventType) {
        case LI_TOUCH_EVENT_HOVER:
        case LI_TOUCH_EVENT_DOWN:
        case LI_TOUCH_EVENT_MOVE:
          {
            // Convert our 0..360 range to -90..90 relative to Y axis
            int adjusted_angle = touch.rotation;

            if (adjusted_angle > 90 && adjusted_angle < 270) {
              // Lower hemisphere
              adjusted_angle = 180 - adjusted_angle;
            }

            // Wrap the value if it's out of range
            if (adjusted_angle > 90) {
              adjusted_angle -= 360;
            } else if (adjusted_angle < -90) {
              adjusted_angle += 360;
            }
            (*raw->touch).place_finger(touch.pointerId, touch.x, touch.y, touch.pressureOrDistance, adjusted_angle);
            break;
          }
        case LI_TOUCH_EVENT_CANCEL:
        case LI_TOUCH_EVENT_UP:
        case LI_TOUCH_EVENT_HOVER_LEAVE:
          {
            (*raw->touch).release_finger(touch.pointerId);
            break;
          }
          // TODO: LI_TOUCH_EVENT_CANCEL_ALL
      }
    }
  }
}  // namespace platf::touch
