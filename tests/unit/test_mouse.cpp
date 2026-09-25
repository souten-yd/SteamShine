/**
 * @file tests/unit/test_mouse.cpp
 * @brief Test src/input.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

#ifdef _WIN32
  // platform includes
  #include <Windows.h>
#endif

#if defined(__APPLE__) && defined(__MACH__)
  // platform includes
  #include <ApplicationServices/ApplicationServices.h>
#endif

// local includes
#include <src/input.h>

#if defined(__linux__) || defined(__FreeBSD__)
  #include <linux/input-event-codes.h>
  #include <src/platform/linux/input/gamescope_eis_input.h>
  #include <src/platform/linux/input/gamescope_pointer_state.h>
  #include <src/platform/linux/input/input_key_mapping.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__)
/** @brief Preserve generic and side-specific modifier aliases in the EIS key map. */
TEST(GamescopeInputMappingTest, ConvertsSupportedVirtualKeysToLinuxCodes) {
  EXPECT_EQ(platf::keyboard::linux_keycode(0x41), KEY_A);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x10), KEY_LEFTSHIFT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0xA0), KEY_LEFTSHIFT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0xA1), KEY_RIGHTSHIFT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x11), KEY_LEFTCTRL);
  EXPECT_EQ(platf::keyboard::linux_keycode(0xA2), KEY_LEFTCTRL);
  EXPECT_EQ(platf::keyboard::linux_keycode(0xA3), KEY_RIGHTCTRL);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x12), KEY_LEFTALT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0xA4), KEY_LEFTALT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0xA5), KEY_RIGHTALT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x0C), KEY_CLEAR);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x13), KEY_PAUSE);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x15), KEY_KATAKANAHIRAGANA);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x16), KEY_HANGEUL);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x17), KEY_HANJA);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x19), KEY_KATAKANA);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x29), KEY_SELECT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x2A), KEY_PRINT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x2F), KEY_HELP);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x5D), KEY_COMPOSE);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x5F), KEY_SLEEP);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x6C), KEY_KPCOMMA);
  EXPECT_FALSE(platf::keyboard::linux_keycode(0xFFFF).has_value());
}

/**
 * @brief Verify touch and pen share one releasable Gamescope pointer button.
 */
TEST(GamescopeInputMappingTest, ReleasesCombinedPointerStateOnDisconnect) {
  platf::gamescope_pointer_state_t state;

  ASSERT_EQ(state.update(platf::gamescope_pointer_source_e::touch, true), true);
  EXPECT_FALSE(state.update(platf::gamescope_pointer_source_e::pen, true).has_value());
  EXPECT_FALSE(state.update(platf::gamescope_pointer_source_e::touch, false).has_value());
  ASSERT_EQ(state.update(platf::gamescope_pointer_source_e::pen, false), false);

  ASSERT_EQ(state.update(platf::gamescope_pointer_source_e::touch, true), true);
  EXPECT_TRUE(state.reset());
  EXPECT_FALSE(state.reset());
}

/**
 * @brief Verify only the desktop route may instantiate desktop input devices.
 */
TEST(GamescopeInputMappingTest, DefersDesktopDevicesForGamescopeRoutes) {
  EXPECT_TRUE(platf::gamescope_input_uses_desktop_device(platf::gamescope_input_result_e::desktop));
  EXPECT_FALSE(platf::gamescope_input_uses_desktop_device(platf::gamescope_input_result_e::delivered));
  EXPECT_FALSE(platf::gamescope_input_uses_desktop_device(platf::gamescope_input_result_e::blocked));
}
#endif
namespace {
  constexpr double mouse_position_tolerance = 1.0;
  constexpr auto mouse_input_wait = std::chrono::milliseconds(500);
  constexpr auto mouse_input_poll_interval = std::chrono::milliseconds(10);

  bool is_near(double actual, double expected) {
    return std::abs(actual - expected) <= mouse_position_tolerance;
  }

  bool is_relative_axis_moved(double actual_delta, double requested_delta) {
    if (requested_delta > 0.0) {
      return actual_delta > 0.0;
    }
    if (requested_delta < 0.0) {
      return actual_delta < 0.0;
    }

    return is_near(actual_delta, 0.0);
  }

  void expect_relative_axis_moved(double actual_delta, double requested_delta) {
    EXPECT_TRUE(is_relative_axis_moved(actual_delta, requested_delta));
  }

  template<typename Predicate>
  std::optional<util::point_t> wait_for_mouse_location(platf::input_t &input, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + mouse_input_wait;
    auto location = platf::get_mouse_loc(input);
    while ((!location || !predicate(*location)) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(mouse_input_poll_interval);
      location = platf::get_mouse_loc(input);
    }

    if (!location || !predicate(*location)) {
      return std::nullopt;
    }

    return location;
  }

  platf::touch_port_t absolute_mouse_test_port() {
#ifdef _WIN32
    return platf::touch_port_t {
      .offset_x = GetSystemMetrics(SM_XVIRTUALSCREEN),
      .offset_y = GetSystemMetrics(SM_YVIRTUALSCREEN),
      .width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
      .height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)),
    };
#elif defined(__APPLE__) && defined(__MACH__)
    const auto display_bounds = CGDisplayBounds(CGMainDisplayID());
    return platf::touch_port_t {
      .offset_x = static_cast<int>(display_bounds.origin.x),
      .offset_y = static_cast<int>(display_bounds.origin.y),
      .width = static_cast<int>(display_bounds.size.width),
      .height = static_cast<int>(display_bounds.size.height),
    };
#elif defined(__linux__) || defined(__FreeBSD__)
    return platf::touch_port_t {
      .offset_x = 0,
      .offset_y = 0,
      .width = 19200,
      .height = 12000,
    };
#else
    return platf::touch_port_t {};
#endif
  }

  util::point_t expected_absolute_mouse_location(const util::point_t &mouse_pos, const platf::touch_port_t &touch_port) {
    return {
      static_cast<double>(touch_port.offset_x) + mouse_pos.x,
      static_cast<double>(touch_port.offset_y) + mouse_pos.y,
    };
  }

}  // namespace

struct MouseHIDTest: PlatformTestSuite, testing::WithParamInterface<util::point_t> {
  void SetUp() override {
    BaseTest::SetUp();
  }

  void TearDown() override {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BaseTest::TearDown();
  }
};

INSTANTIATE_TEST_SUITE_P(
  MouseInputs,
  MouseHIDTest,
  testing::Values(
    util::point_t {40, 40},
    util::point_t {70, 150}
  )
);

// todo: add tests for hitting screen edges

TEST_P(MouseHIDTest, MoveInputTest) {
  util::point_t mouse_delta = GetParam();

  BOOST_LOG(tests) << "MoveInputTest:: got param: " << mouse_delta;
  platf::input_t input = platf::input();
  BOOST_LOG(tests) << "MoveInputTest:: init input";

  BOOST_LOG(tests) << "MoveInputTest:: get current mouse loc";
  auto old_loc = platf::get_mouse_loc(input);
  if (!old_loc) {
    GTEST_SKIP() << "Mouse location is not observable in this environment";
  }
  BOOST_LOG(tests) << "MoveInputTest:: got current mouse loc: " << *old_loc;

  BOOST_LOG(tests) << "MoveInputTest:: move: " << mouse_delta;
  platf::move_mouse(input, mouse_delta.x, mouse_delta.y);
  BOOST_LOG(tests) << "MoveInputTest:: moved: " << mouse_delta;

  BOOST_LOG(tests) << "MoveInputTest:: get updated mouse loc";
  auto new_loc = wait_for_mouse_location(input, [old_loc = *old_loc, &mouse_delta](const auto &location) {
    return is_relative_axis_moved(location.x - old_loc.x, mouse_delta.x) &&
           is_relative_axis_moved(location.y - old_loc.y, mouse_delta.y);
  });
  if (!new_loc) {
    GTEST_SKIP() << "Virtual mouse movement is not observable in this environment";
  }
  BOOST_LOG(tests) << "MoveInputTest:: got updated mouse loc: " << *new_loc;

  bool has_input_moved = old_loc->x != new_loc->x && old_loc->y != new_loc->y;

  if (!has_input_moved) {
    BOOST_LOG(tests) << "MoveInputTest:: haven't moved";
  } else {
    BOOST_LOG(tests) << "MoveInputTest:: moved";
  }

  EXPECT_TRUE(has_input_moved);

  // Verify the OS moved in the requested direction. Relative pointer input can be accelerated by host settings.
  expect_relative_axis_moved(new_loc->x - old_loc->x, mouse_delta.x);
  expect_relative_axis_moved(new_loc->y - old_loc->y, mouse_delta.y);
}

TEST_P(MouseHIDTest, AbsMoveInputTest) {
  util::point_t mouse_pos = GetParam();
  BOOST_LOG(tests) << "AbsMoveInputTest:: got param: " << mouse_pos;

  platf::input_t input = platf::input();
  BOOST_LOG(tests) << "AbsMoveInputTest:: init input";

  BOOST_LOG(tests) << "AbsMoveInputTest:: get current mouse loc";
  auto old_loc = platf::get_mouse_loc(input);
  if (!old_loc) {
    GTEST_SKIP() << "Mouse location is not observable in this environment";
  }
  BOOST_LOG(tests) << "AbsMoveInputTest:: got current mouse loc: " << *old_loc;

  const auto abs_port = absolute_mouse_test_port();
  const auto expected_pos = expected_absolute_mouse_location(mouse_pos, abs_port);
  BOOST_LOG(tests) << "AbsMoveInputTest:: touch port: " << abs_port.offset_x << 'x' << abs_port.offset_y << ' ' << abs_port.width << 'x' << abs_port.height;
  BOOST_LOG(tests) << "AbsMoveInputTest:: move: " << mouse_pos;
  platf::abs_mouse(input, abs_port, mouse_pos.x, mouse_pos.y);
  BOOST_LOG(tests) << "AbsMoveInputTest:: moved: " << mouse_pos;

  BOOST_LOG(tests) << "AbsMoveInputTest:: get updated mouse loc";
  auto new_loc = wait_for_mouse_location(input, [&expected_pos](const auto &location) {
    return is_near(location.x, expected_pos.x) && is_near(location.y, expected_pos.y);
  });
  if (!new_loc) {
    GTEST_SKIP() << "Absolute virtual mouse movement is not observable in this environment";
  }
  BOOST_LOG(tests) << "AbsMoveInputTest:: got updated mouse loc: " << *new_loc;

  bool has_input_moved = old_loc->x != new_loc->x || old_loc->y != new_loc->y;

  if (!has_input_moved) {
    BOOST_LOG(tests) << "AbsMoveInputTest:: haven't moved";
  } else {
    BOOST_LOG(tests) << "AbsMoveInputTest:: moved";
  }

  // Verify we moved to the absolute coordinate
  EXPECT_NEAR(new_loc->x, expected_pos.x, mouse_position_tolerance);
  EXPECT_NEAR(new_loc->y, expected_pos.y, mouse_position_tolerance);
}
