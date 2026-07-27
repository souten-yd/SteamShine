/**
 * @file src/platform/linux/input/gamescope_eis_input.h
 * @brief Session-scoped input delivery to a verified Gamescope EIS endpoint.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace platf {
  /**
   * @brief Result of attempting to route an input event to Gamescope.
   */
  enum class gamescope_input_result_e {
    desktop,  ///< No Gamescope session is selected; use the normal desktop backend.
    delivered,  ///< The event was delivered to the selected Gamescope session.
    blocked,  ///< Gamescope is selected but safe delivery was unavailable.
  };

  /**
   * @brief Dynamically loaded libei sender for the verified Gamescope session.
   */
  class gamescope_eis_input_t {
  public:
    /**
     * @brief Construct a disconnected Gamescope input sender.
     */
    gamescope_eis_input_t();

    /**
     * @brief Release all libei resources and emulated input state.
     */
    ~gamescope_eis_input_t();

    gamescope_eis_input_t(const gamescope_eis_input_t &) = delete;
    gamescope_eis_input_t &operator=(const gamescope_eis_input_t &) = delete;

    /**
     * @brief Prepare the sender for the currently selected stream source.
     *
     * @return Desktop fallback, ready delivery, or fail-closed blocking.
     */
    gamescope_input_result_e refresh();

    /**
     * @brief Deliver relative pointer motion.
     *
     * @param delta_x Horizontal motion in logical pixels.
     * @param delta_y Vertical motion in logical pixels.
     * @return Routing result.
     */
    gamescope_input_result_e move(int delta_x, int delta_y);

    /**
     * @brief Deliver absolute pointer motion.
     *
     * @param x Horizontal position in Gamescope logical pixels.
     * @param y Vertical position in Gamescope logical pixels.
     * @return Routing result.
     */
    gamescope_input_result_e move_absolute(float x, float y);

    /**
     * @brief Deliver a Linux input button transition.
     *
     * @param button Linux `BTN_*` code.
     * @param pressed True for press and false for release.
     * @return Routing result.
     */
    gamescope_input_result_e button(std::uint32_t button, bool pressed);

    /**
     * @brief Deliver a discrete scroll event.
     *
     * @param horizontal Horizontal distance in 120-unit wheel clicks.
     * @param vertical Vertical distance in 120-unit wheel clicks.
     * @return Routing result.
     */
    gamescope_input_result_e scroll(std::int32_t horizontal, std::int32_t vertical);

    /**
     * @brief Deliver a Linux key transition.
     *
     * @param key Linux `KEY_*` code.
     * @param pressed True for press and false for release.
     * @return Routing result.
     */
    gamescope_input_result_e key(std::uint32_t key, bool pressed);

    /**
     * @brief Return the stable diagnostic reason for the current state.
     *
     * @return Empty string when no failure is active.
     */
    std::string error() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Hidden dynamic libei implementation.
  };
}  // namespace platf
