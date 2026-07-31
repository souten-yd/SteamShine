/**
 * @file tests/unit/test_mouse.cpp
 * @brief Test src/input.*.
 */
#include "../tests_common.h"

#include <src/input.h>

#if defined(__linux__) || defined(__FreeBSD__)
  #include <linux/input-event-codes.h>
  #include <src/platform/linux/input/gamescope_eis_input.h>
  #include <src/platform/linux/input/gamescope_pointer_state.h>
  #include <src/platform/linux/input/input_key_mapping.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__)
TEST(GamescopeInputMappingTest, ConvertsSupportedVirtualKeysToLinuxCodes) {
  EXPECT_EQ(platf::keyboard::linux_keycode(0x41), KEY_A);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x10), KEY_LEFTSHIFT);
  EXPECT_EQ(platf::keyboard::linux_keycode(0x12), KEY_LEFTALT);
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

struct MouseHIDTest: PlatformTestSuite, testing::WithParamInterface<util::point_t> {
  void SetUp() override {
    BaseTest::SetUp();
#ifdef _WIN32
    // TODO: Windows tests are failing, `get_mouse_loc` seems broken and `platf::abs_mouse` too
    //       the alternative `platf::abs_mouse` method seem to work better during tests,
    //       but I'm not sure about real work
    GTEST_SKIP() << "TODO Windows";
#elif defined(__linux__) || defined(__FreeBSD__)
    // TODO: Inputtino waiting https://github.com/games-on-whales/inputtino/issues/6 is resolved.
    GTEST_SKIP() << "TODO Inputtino";
#endif
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
  BOOST_LOG(tests) << "MoveInputTest:: got current mouse loc: " << old_loc;

  BOOST_LOG(tests) << "MoveInputTest:: move: " << mouse_delta;
  platf::move_mouse(input, mouse_delta.x, mouse_delta.y);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  BOOST_LOG(tests) << "MoveInputTest:: moved: " << mouse_delta;

  BOOST_LOG(tests) << "MoveInputTest:: get updated mouse loc";
  auto new_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "MoveInputTest:: got updated mouse loc: " << new_loc;

  bool has_input_moved = old_loc.x != new_loc.x && old_loc.y != new_loc.y;

  if (!has_input_moved) {
    BOOST_LOG(tests) << "MoveInputTest:: haven't moved";
  } else {
    BOOST_LOG(tests) << "MoveInputTest:: moved";
  }

  EXPECT_TRUE(has_input_moved);

  // Verify we moved as much as we requested
  EXPECT_EQ(new_loc.x - old_loc.x, mouse_delta.x);
  EXPECT_EQ(new_loc.y - old_loc.y, mouse_delta.y);
}

TEST_P(MouseHIDTest, AbsMoveInputTest) {
  util::point_t mouse_pos = GetParam();
  BOOST_LOG(tests) << "AbsMoveInputTest:: got param: " << mouse_pos;

  platf::input_t input = platf::input();
  BOOST_LOG(tests) << "AbsMoveInputTest:: init input";

  BOOST_LOG(tests) << "AbsMoveInputTest:: get current mouse loc";
  auto old_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "AbsMoveInputTest:: got current mouse loc: " << old_loc;

#ifdef _WIN32
  platf::touch_port_t abs_port {
    0,
    0,
    65535,
    65535
  };
#elif defined(__linux__) || defined(__FreeBSD__)
  platf::touch_port_t abs_port {
    0,
    0,
    19200,
    12000
  };
#else
  platf::touch_port_t abs_port {};
#endif
  BOOST_LOG(tests) << "AbsMoveInputTest:: move: " << mouse_pos;
  platf::abs_mouse(input, abs_port, mouse_pos.x, mouse_pos.y);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  BOOST_LOG(tests) << "AbsMoveInputTest:: moved: " << mouse_pos;

  BOOST_LOG(tests) << "AbsMoveInputTest:: get updated mouse loc";
  auto new_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "AbsMoveInputTest:: got updated mouse loc: " << new_loc;

  bool has_input_moved = old_loc.x != new_loc.x || old_loc.y != new_loc.y;

  if (!has_input_moved) {
    BOOST_LOG(tests) << "AbsMoveInputTest:: haven't moved";
  } else {
    BOOST_LOG(tests) << "AbsMoveInputTest:: moved";
  }

  EXPECT_TRUE(has_input_moved);

  // Verify we moved to the absolute coordinate
  EXPECT_EQ(new_loc.x, mouse_pos.x);
  EXPECT_EQ(new_loc.y, mouse_pos.y);
}
