/**
 * @file tests/unit/test_stream_recording.cpp
 * @brief Tests for sender recording settings and bounded retention.
 */

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"

#include <src/stream_recording.h>

namespace {
  /**
   * @brief Own one unique temporary recording directory.
   */
  class temporary_recording_root_t {
  public:
    /**
     * @brief Create a unique owner-private test directory.
     */
    temporary_recording_root_t() {
      const auto suffix {std::chrono::steady_clock::now().time_since_epoch().count()};
      path = std::filesystem::temp_directory_path() / ("steamshine-recording-test-" + std::to_string(suffix));
      std::filesystem::create_directories(path);
    }

    /**
     * @brief Remove every test artifact.
     */
    ~temporary_recording_root_t() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;  ///< Unique test directory.
  };

  /**
   * @brief Create one sparse completed recording with a deterministic size.
   *
   * @param path Destination MP4 path.
   * @param size Requested file size in bytes.
   */
  void create_recording(const std::filesystem::path &path, const std::uint64_t size) {
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.seekp(static_cast<std::streamoff>(size - 1U));
    output.put('\0');
    ASSERT_TRUE(output);
  }
}  // namespace

TEST(StreamRecordingTest, DefaultsToFiveHundredMegabytesAndValidatesSettings) {
  temporary_recording_root_t root;
  stream_recording::Service service {root.path};

  const auto initial = service.snapshot();
  EXPECT_EQ(initial.at("capacity_mb"), 500);
  EXPECT_EQ(initial.at("state"), "idle");
  EXPECT_FALSE(service.set_capacity_megabytes(0).success);
  EXPECT_FALSE(service.set_capacity_megabytes(102401).success);
  EXPECT_TRUE(service.set_capacity_megabytes(750).success);
  EXPECT_EQ(service.snapshot().at("capacity_mb"), 750);

  stream_recording::Service reloaded {root.path};
  EXPECT_EQ(reloaded.snapshot().at("capacity_mb"), 750);
}

TEST(StreamRecordingTest, RetentionPreservesTheOnlyRecordingEvenWhenItExceedsCapacity) {
  temporary_recording_root_t root;
  const auto oldest {root.path / "20260730-100000-0.mp4"};
  const auto newest {root.path / "20260730-100001-0.mp4"};
  create_recording(oldest, 2U * 1024U * 1024U);
  create_recording(newest, 2U * 1024U * 1024U);
  std::filesystem::last_write_time(oldest, std::filesystem::file_time_type::clock::now() - std::chrono::minutes {1});

  stream_recording::Service service {root.path};
  ASSERT_TRUE(service.set_capacity_megabytes(1).success);

  EXPECT_FALSE(std::filesystem::exists(oldest));
  EXPECT_TRUE(std::filesystem::exists(newest));
  EXPECT_GT(service.snapshot().at("used_bytes").get<std::uint64_t>(), 1024U * 1024U);
}

TEST(StreamRecordingTest, NextRecordingDeletesAnOversizedSolePredecessorBeforeStarting) {
  temporary_recording_root_t root;
  const auto predecessor {root.path / "20260730-100000-0.mp4"};
  const auto muxer {root.path / "fake-ffmpeg"};
  create_recording(predecessor, 2U * 1024U * 1024U);
  {
    std::ofstream output {muxer};
    ASSERT_TRUE(output);
    output << "#!/bin/sh\ncat >/dev/null\n";
  }
  std::filesystem::permissions(muxer, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);

  stream_recording::Service service {root.path, muxer.string()};
  ASSERT_TRUE(service.set_capacity_megabytes(1).success);
  ASSERT_TRUE(std::filesystem::exists(predecessor));
  ASSERT_TRUE(service.set_enabled(true).success);
  const std::vector<std::uint8_t> bytes {0, 0, 0, 1, 0x65, 0};
  service.submit({
    .payload = bytes,
    .stream_id = 1,
    .frame_index = 1,
    .codec = 0,
    .width = 1920,
    .height = 1080,
    .fps = 60,
    .hdr = false,
    .idr = true,
  });

  for (int attempt = 0; attempt < 100 && std::filesystem::exists(predecessor); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }
  EXPECT_FALSE(std::filesystem::exists(predecessor));
  for (int attempt = 0; attempt < 100 && service.snapshot().at("state") != "recording"; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }
  ASSERT_EQ(service.snapshot().at("state"), "recording");
  service.stream_ended(2);
  EXPECT_TRUE(service.snapshot().at("enabled"));
  service.stream_ended(1);
  EXPECT_FALSE(service.snapshot().at("enabled"));
}

TEST(StreamRecordingTest, ResolvesAndDeletesOnlyGeneratedIdentifiers) {
  temporary_recording_root_t root;
  const std::string id {"20260730-100000-0"};
  create_recording(root.path / (id + ".mp4"), 32);
  std::ofstream {root.path / (id + ".json")} << "{}";
  std::ofstream {root.path / (id + ".log")} << "log";
  stream_recording::Service service {root.path};

  EXPECT_EQ(service.resolve(id), root.path / (id + ".mp4"));
  EXPECT_TRUE(service.resolve("../outside").empty());
  EXPECT_FALSE(service.remove("../outside").success);
  EXPECT_TRUE(service.remove(id).success);
  EXPECT_FALSE(std::filesystem::exists(root.path / (id + ".mp4")));
  EXPECT_FALSE(std::filesystem::exists(root.path / (id + ".json")));
  EXPECT_FALSE(std::filesystem::exists(root.path / (id + ".log")));
}
