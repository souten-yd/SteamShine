/**
 * @file tests/unit/test_input_queue.cpp
 * @brief Unit tests for bounded, edge-preserving input packet scheduling.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include "src/input.h"
#include "src/utility.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace {
  /**
   * @brief Verify controller diagnostics identify button and stick hold releases.
   */
  TEST(InputGamepadDiagnostics, DetectsHeldControlReleaseEdges) {
    platf::gamepad_state_t held {};
    held.buttonFlags = platf::A | platf::LEFT_BUTTON;
    held.lsX = 18000;
    held.rsY = -22000;

    platf::gamepad_state_t partial_release {held};
    partial_release.buttonFlags = platf::LEFT_BUTTON;
    partial_release.lsX = 12000;
    partial_release.rsY = 0;
    const auto partial {input::detect_gamepad_hold_release(held, partial_release)};
    EXPECT_EQ(partial.released_buttons, platf::A);
    EXPECT_FALSE(partial.left_stick_released);
    EXPECT_TRUE(partial.right_stick_released);

    platf::gamepad_state_t neutral {partial_release};
    neutral.lsX = 500;
    const auto final {input::detect_gamepad_hold_release(partial_release, neutral)};
    EXPECT_EQ(final.released_buttons, 0U);
    EXPECT_TRUE(final.left_stick_released);
    EXPECT_FALSE(final.right_stick_released);

    platf::gamepad_state_t redirected {held};
    redirected.lsX = -18000;
    redirected.rsY = 22000;
    EXPECT_FALSE(input::detect_gamepad_hold_release(held, redirected).any());
  }

  /**
   * @brief Copy a packed Moonlight input structure into queue-owned bytes.
   *
   * @tparam Packet Concrete Moonlight input packet type.
   * @param packet Packet to copy.
   * @return Byte representation accepted by @ref input::packet_queue_t.
   */
  template<class Packet>
  std::vector<std::uint8_t> packet_bytes(const Packet &packet) {
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }

  /**
   * @brief Construct a relative mouse-motion packet.
   *
   * @param x Horizontal delta.
   * @param y Vertical delta.
   * @return Encoded relative-motion packet.
   */
  std::vector<std::uint8_t> relative_motion(const std::int16_t x, const std::int16_t y) {
    NV_REL_MOUSE_MOVE_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(MOUSE_MOVE_REL_MAGIC_GEN5);
    packet.deltaX = util::endian::big(x);
    packet.deltaY = util::endian::big(y);
    return packet_bytes(packet);
  }

  /**
   * @brief Construct an absolute pointer-motion packet.
   *
   * @param x Horizontal coordinate.
   * @param y Vertical coordinate.
   * @return Encoded absolute-motion packet.
   */
  std::vector<std::uint8_t> absolute_motion(const std::int16_t x, const std::int16_t y) {
    NV_ABS_MOUSE_MOVE_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(MOUSE_MOVE_ABS_MAGIC);
    packet.x = x;
    packet.y = y;
    packet.width = 1920;
    packet.height = 1080;
    return packet_bytes(packet);
  }

  /**
   * @brief Construct a touch transition or latest-state move packet.
   *
   * @param event_type Moonlight touch event type.
   * @param pointer_id Stable contact identifier.
   * @param marker Raw coordinate marker used to identify the retained packet.
   * @return Encoded touch packet.
   */
  std::vector<std::uint8_t> touch_packet(
    const std::uint8_t event_type,
    const std::uint32_t pointer_id,
    const std::uint32_t marker
  ) {
    SS_TOUCH_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(SS_TOUCH_MAGIC);
    packet.eventType = event_type;
    packet.pointerId = pointer_id;
    std::memcpy(&packet.x, &marker, sizeof(marker));
    return packet_bytes(packet);
  }

  /**
   * @brief Construct a controller state containing axes and button edges.
   *
   * @param controller Controller index.
   * @param buttons Raw button flags.
   * @param axis Left-stick horizontal axis marker.
   * @return Encoded controller-state packet.
   */
  std::vector<std::uint8_t> controller_state(
    const std::int16_t controller,
    const std::int16_t buttons,
    const std::int16_t axis
  ) {
    NV_MULTI_CONTROLLER_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(MULTI_CONTROLLER_MAGIC_GEN5);
    packet.controllerNumber = controller;
    packet.activeGamepadMask = 0x0003;
    packet.buttonFlags = buttons;
    packet.leftStickX = axis;
    return packet_bytes(packet);
  }

  /**
   * @brief Construct a controller motion-sensor packet.
   *
   * @param controller Controller index.
   * @param motion_type Sensor identifier.
   * @param marker Raw sample marker.
   * @return Encoded controller motion packet.
   */
  std::vector<std::uint8_t> controller_motion(
    const std::uint8_t controller,
    const std::uint8_t motion_type,
    const std::uint32_t marker
  ) {
    SS_CONTROLLER_MOTION_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(SS_CONTROLLER_MOTION_MAGIC);
    packet.controllerNumber = controller;
    packet.motionType = motion_type;
    std::memcpy(&packet.x, &marker, sizeof(marker));
    return packet_bytes(packet);
  }

  /**
   * @brief Construct a keyboard edge packet.
   *
   * @param down True for key-down and false for key-up.
   * @param key Virtual key code.
   * @return Encoded keyboard packet.
   */
  std::vector<std::uint8_t> key_edge(const bool down, const std::int16_t key) {
    NV_KEYBOARD_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(down ? KEY_DOWN_EVENT_MAGIC : KEY_UP_EVENT_MAGIC);
    packet.keyCode = util::endian::big(key);
    return packet_bytes(packet);
  }

  /**
   * @brief Read host-endian packet magic from queue output.
   *
   * @param packet Queue output packet.
   * @return Host-endian packet magic.
   */
  std::uint32_t packet_magic(const input::queued_packet_t &packet) {
    const auto *const header {reinterpret_cast<const NV_INPUT_HEADER *>(packet.data.data())};
    return util::endian::little(header->magic);
  }

  /**
   * @brief Verify high-rate relative motion collapses into one scheduled packet.
   */
  TEST(InputPacketQueue, CoalescesContinuousRelativeMotion) {
    input::packet_queue_t queue {8};

    const auto first {queue.push(relative_motion(1, -1))};
    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.schedule_worker);

    std::uint64_t coalesced {};
    for (int index = 1; index < 1000; ++index) {
      const auto result {queue.push(relative_motion(1, -1))};
      EXPECT_TRUE(result.accepted);
      EXPECT_FALSE(result.schedule_worker);
      coalesced += result.coalesced;
    }

    EXPECT_EQ(queue.size(), 1);
    EXPECT_EQ(coalesced, 999);
    const auto packet {queue.pop()};
    ASSERT_TRUE(packet);
    const auto *const motion {reinterpret_cast<const NV_REL_MOUSE_MOVE_PACKET *>(packet->data.data())};
    EXPECT_EQ(util::endian::big(motion->deltaX), 1000);
    EXPECT_EQ(util::endian::big(motion->deltaY), -1000);
    EXPECT_FALSE(queue.worker_finished());
  }

  /**
   * @brief Verify sixty seconds of 1 kHz motion never creates extra worker jobs.
   */
  TEST(InputPacketQueue, BoundsSixtySecondEquivalentMotionBurst) {
    input::packet_queue_t queue {64};
    std::size_t worker_requests {};
    std::uint64_t coalesced {};

    for (int event {}; event < 60 * 1000; ++event) {
      const auto result {queue.push(relative_motion(1, 0))};
      ASSERT_TRUE(result.accepted);
      worker_requests += result.schedule_worker ? 1U : 0U;
      coalesced += result.coalesced;
    }

    EXPECT_EQ(worker_requests, 1);
    EXPECT_EQ(queue.size(), 2);
    EXPECT_EQ(coalesced, 59998);
    const auto first {queue.pop()};
    ASSERT_TRUE(first);
    const auto second {queue.pop()};
    ASSERT_TRUE(second);
    EXPECT_EQ(packet_magic(*first), MOUSE_MOVE_REL_MAGIC_GEN5);
    EXPECT_EQ(packet_magic(*second), MOUSE_MOVE_REL_MAGIC_GEN5);
    EXPECT_FALSE(queue.worker_finished());
  }

  /**
   * @brief Exercise a real-time 1 kHz producer for sixty seconds.
   *
   * A deliberately slower consumer verifies that motion coalescing keeps the
   * queue bounded, key edges survive the load, and a final edge becomes
   * observable within the recovery target once production stops.
   */
  TEST(InputPacketQueue, SustainsRealtimeSixtySecondProducer) {
    input::packet_queue_t queue {64};
    std::atomic<bool> stop_consumer {false};
    std::atomic<std::size_t> maximum_depth {};
    std::atomic<std::uint64_t> key_edges {};
    std::promise<std::chrono::steady_clock::time_point> final_edge_seen;
    auto final_edge_future {final_edge_seen.get_future()};

    std::jthread consumer {[&](const std::stop_token stop_token) {
      bool final_reported {false};
      while (!stop_token.stop_requested() &&
             (!stop_consumer.load(std::memory_order_relaxed) || queue.size() != 0)) {
        if (auto packet = queue.pop()) {
          const auto magic {packet_magic(*packet)};
          if (magic == KEY_DOWN_EVENT_MAGIC || magic == KEY_UP_EVENT_MAGIC) {
            ++key_edges;
            const auto *const key {reinterpret_cast<const NV_KEYBOARD_PACKET *>(packet->data.data())};
            if (!final_reported && util::endian::big(key->keyCode) == 0x42) {
              final_edge_seen.set_value(std::chrono::steady_clock::now());
              final_reported = true;
            }
          }
        }
        std::this_thread::sleep_for(4ms);
      }
    }};

    const auto deadline {std::chrono::steady_clock::now() + 60s};
    std::uint64_t event {};
    while (std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(queue.push(relative_motion(1, 0)).accepted);
      if (++event % 250 == 0) {
        ASSERT_TRUE(queue.push(key_edge(true, 0x41)).accepted);
        ASSERT_TRUE(queue.push(key_edge(false, 0x41)).accepted);
      }
      maximum_depth.store(std::max(maximum_depth.load(std::memory_order_relaxed), queue.size()), std::memory_order_relaxed);
      std::this_thread::sleep_for(1ms);
    }

    const auto recovery_started {std::chrono::steady_clock::now()};
    ASSERT_TRUE(queue.push(key_edge(true, 0x42)).accepted);
    ASSERT_EQ(final_edge_future.wait_for(100ms), std::future_status::ready);
    const auto recovered_at {final_edge_future.get()};
    stop_consumer.store(true, std::memory_order_relaxed);
    consumer.join();

    EXPECT_LE(maximum_depth.load(std::memory_order_relaxed), 64U);
    EXPECT_GT(key_edges.load(std::memory_order_relaxed), 100U);
    EXPECT_LT(recovered_at - recovery_started, 100ms);
  }

  /**
   * @brief Verify motion coalescing never crosses keyboard edges.
   */
  TEST(InputPacketQueue, PreservesEdgesAroundCoalescedMotion) {
    input::packet_queue_t queue {8};

    queue.push(key_edge(true, 0x41));
    queue.push(relative_motion(2, 3));
    const auto coalesced {queue.push(relative_motion(4, 5))};
    queue.push(key_edge(false, 0x41));

    EXPECT_EQ(coalesced.coalesced, 1);
    ASSERT_EQ(queue.size(), 3);

    const auto down {queue.pop()};
    const auto motion {queue.pop()};
    const auto up {queue.pop()};
    ASSERT_TRUE(down);
    ASSERT_TRUE(motion);
    ASSERT_TRUE(up);
    EXPECT_EQ(packet_magic(*down), KEY_DOWN_EVENT_MAGIC);
    EXPECT_EQ(packet_magic(*motion), MOUSE_MOVE_REL_MAGIC_GEN5);
    EXPECT_EQ(packet_magic(*up), KEY_UP_EVENT_MAGIC);

    const auto *const combined {reinterpret_cast<const NV_REL_MOUSE_MOVE_PACKET *>(motion->data.data())};
    EXPECT_EQ(util::endian::big(combined->deltaX), 6);
    EXPECT_EQ(util::endian::big(combined->deltaY), 8);
  }

  /**
   * @brief Verify absolute motion keeps only the newest compatible position.
   */
  TEST(InputPacketQueue, CoalescesAbsolutePointerToLatestState) {
    input::packet_queue_t queue {8};
    ASSERT_TRUE(queue.push(absolute_motion(10, 20)).accepted);
    const auto result {queue.push(absolute_motion(30, 40))};

    EXPECT_EQ(result.coalesced, 1U);
    ASSERT_EQ(queue.size(), 1U);
    const auto output {queue.pop()};
    ASSERT_TRUE(output);
    const auto *const packet {reinterpret_cast<const NV_ABS_MOUSE_MOVE_PACKET *>(output->data.data())};
    EXPECT_EQ(packet->x, 30);
    EXPECT_EQ(packet->y, 40);
  }

  /**
   * @brief Verify touch moves coalesce by pointer ID without crossing edges.
   */
  TEST(InputPacketQueue, CoalescesTouchByContactAndPreservesEnd) {
    input::packet_queue_t queue {8};
    ASSERT_TRUE(queue.push(touch_packet(LI_TOUCH_EVENT_MOVE, 11, 1)).accepted);
    ASSERT_TRUE(queue.push(touch_packet(LI_TOUCH_EVENT_MOVE, 22, 2)).accepted);
    EXPECT_EQ(queue.push(touch_packet(LI_TOUCH_EVENT_MOVE, 11, 3)).coalesced, 1U);
    ASSERT_TRUE(queue.push(touch_packet(LI_TOUCH_EVENT_UP, 11, 4)).accepted);

    ASSERT_EQ(queue.size(), 3U);
    const auto contact_11 {queue.pop()};
    const auto contact_22 {queue.pop()};
    const auto contact_end {queue.pop()};
    ASSERT_TRUE(contact_11);
    ASSERT_TRUE(contact_22);
    ASSERT_TRUE(contact_end);
    const auto *const first {reinterpret_cast<const SS_TOUCH_PACKET *>(contact_11->data.data())};
    const auto *const second {reinterpret_cast<const SS_TOUCH_PACKET *>(contact_22->data.data())};
    const auto *const edge {reinterpret_cast<const SS_TOUCH_PACKET *>(contact_end->data.data())};
    std::uint32_t first_marker {};
    std::memcpy(&first_marker, &first->x, sizeof(first_marker));
    EXPECT_EQ(first->pointerId, 11U);
    EXPECT_EQ(first_marker, 3U);
    EXPECT_EQ(second->pointerId, 22U);
    EXPECT_EQ(edge->eventType, LI_TOUCH_EVENT_UP);
  }

  /**
   * @brief Verify controller axes and sensors coalesce per controller and sensor.
   */
  TEST(InputPacketQueue, CoalescesControllerMotionByDeviceAndAxis) {
    input::packet_queue_t queue {12};
    ASSERT_TRUE(queue.push(controller_state(0, 0, 10)).accepted);
    ASSERT_TRUE(queue.push(controller_state(1, 0, 20)).accepted);
    EXPECT_EQ(queue.push(controller_state(0, 0, 30)).coalesced, 1U);
    ASSERT_TRUE(queue.push(controller_state(0, 1, 40)).accepted);
    ASSERT_TRUE(queue.push(controller_motion(0, 1, 100)).accepted);
    ASSERT_TRUE(queue.push(controller_motion(0, 2, 200)).accepted);
    EXPECT_EQ(queue.push(controller_motion(0, 1, 300)).coalesced, 1U);

    ASSERT_EQ(queue.size(), 5U);
    const auto controller_zero {queue.pop()};
    const auto controller_one {queue.pop()};
    const auto button_edge {queue.pop()};
    const auto sensor_one {queue.pop()};
    const auto sensor_two {queue.pop()};
    ASSERT_TRUE(controller_zero);
    ASSERT_TRUE(controller_one);
    ASSERT_TRUE(button_edge);
    ASSERT_TRUE(sensor_one);
    ASSERT_TRUE(sensor_two);
    EXPECT_EQ(reinterpret_cast<const NV_MULTI_CONTROLLER_PACKET *>(controller_zero->data.data())->leftStickX, 30);
    EXPECT_EQ(reinterpret_cast<const NV_MULTI_CONTROLLER_PACKET *>(controller_one->data.data())->controllerNumber, 1);
    EXPECT_EQ(reinterpret_cast<const NV_MULTI_CONTROLLER_PACKET *>(button_edge->data.data())->buttonFlags, 1);
    std::uint32_t sensor_marker {};
    const auto *const sensor {reinterpret_cast<const SS_CONTROLLER_MOTION_PACKET *>(sensor_one->data.data())};
    std::memcpy(&sensor_marker, &sensor->x, sizeof(sensor_marker));
    EXPECT_EQ(sensor->motionType, 1U);
    EXPECT_EQ(sensor_marker, 300U);
    EXPECT_EQ(reinterpret_cast<const SS_CONTROLLER_MOTION_PACKET *>(sensor_two->data.data())->motionType, 2U);
  }

  /**
   * @brief Verify overflow discards stale motion before any edge packet.
   */
  TEST(InputPacketQueue, DropsOnlyMotionAtCapacity) {
    input::packet_queue_t queue {3};

    queue.push(key_edge(true, 0x41));
    queue.push(relative_motion(7, 9));
    queue.push(key_edge(false, 0x41));
    const auto overflow {queue.push(key_edge(true, 0x42))};

    EXPECT_TRUE(overflow.accepted);
    EXPECT_EQ(overflow.dropped, 1);
    ASSERT_EQ(queue.size(), 3);

    const auto first {queue.pop()};
    const auto second {queue.pop()};
    const auto third {queue.pop()};
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);
    EXPECT_EQ(packet_magic(*first), KEY_DOWN_EVENT_MAGIC);
    EXPECT_EQ(packet_magic(*second), KEY_UP_EVENT_MAGIC);
    EXPECT_EQ(packet_magic(*third), KEY_DOWN_EVENT_MAGIC);
  }

  /**
   * @brief Verify an edge-only overflow applies bounded producer backpressure.
   */
  TEST(InputPacketQueue, BlocksRatherThanDroppingEdges) {
    input::packet_queue_t queue {2};
    queue.push(key_edge(true, 0x41));
    queue.push(key_edge(false, 0x41));

    auto producer = std::async(std::launch::async, [&queue]() {
      return queue.push(key_edge(true, 0x42));
    });

    EXPECT_EQ(producer.wait_for(20ms), std::future_status::timeout);
    ASSERT_TRUE(queue.pop());
    EXPECT_EQ(producer.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(producer.get().accepted);
    EXPECT_EQ(queue.size(), 2);
  }

  /**
   * @brief Verify stopping a stream clears stale input and releases blocked producers.
   */
  TEST(InputPacketQueue, StopClearsBacklogAndUnblocksProducer) {
    input::packet_queue_t queue {1};
    queue.push(key_edge(true, 0x41));

    auto producer = std::async(std::launch::async, [&queue]() {
      return queue.push(key_edge(false, 0x41));
    });
    EXPECT_EQ(producer.wait_for(20ms), std::future_status::timeout);

    queue.stop();
    EXPECT_EQ(producer.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(producer.get().accepted);
    EXPECT_EQ(queue.size(), 0);
    EXPECT_FALSE(queue.pop());
  }

  /**
   * @brief Verify fixed-memory latency statistics retain bounded percentiles.
   */
  TEST(InputPacketQueue, AggregatesLatencyWithoutDynamicEventStorage) {
    latency_diagnostics::fixed_ring_t<3> statistics;
    statistics.record(1ms);
    statistics.record(2ms);
    statistics.record(3ms);
    statistics.record(4ms);

    const auto snapshot {statistics.snapshot()};
    EXPECT_EQ(snapshot.count, 4U);
    EXPECT_EQ(snapshot.window_count, 3U);
    EXPECT_DOUBLE_EQ(snapshot.average_ms, 2.5);
    EXPECT_DOUBLE_EQ(snapshot.p50_ms, 3.0);
    EXPECT_DOUBLE_EQ(snapshot.p95_ms, 4.0);
    EXPECT_DOUBLE_EQ(snapshot.p99_ms, 4.0);
    EXPECT_DOUBLE_EQ(snapshot.max_ms, 4.0);
  }

  /**
   * @brief Verify the ordered network policy bounds a producer without clearing frames.
   */
  TEST(InputPacketQueue, BlocksBoundedOrderedQueueWithoutDiscardingValues) {
    safe::queue_t<int> queue {2, safe::queue_overflow_e::block_producer};
    queue.raise(1);
    queue.raise(2);

    auto producer = std::async(std::launch::async, [&queue]() {
      queue.raise(3);
    });
    EXPECT_EQ(producer.wait_for(20ms), std::future_status::timeout);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.pop(1s), 1);
    EXPECT_EQ(producer.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.pop(1s), 2);
    EXPECT_EQ(queue.pop(1s), 3);
  }
}  // namespace
