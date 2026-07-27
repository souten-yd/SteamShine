/**
 * @file tests/unit/platform/test_common.cpp
 * @brief Test src/platform/common.*.
 */
#include "../../tests_common.h"

#include <boost/asio/ip/host_name.hpp>
#include <src/platform/common.h>

TEST(HostnameTests, TestAsioEquality) {
  // These should be equivalent on all platforms for ASCII hostnames
  ASSERT_EQ(platf::get_host_name(), boost::asio::ip::host_name());
}

TEST(ImageFallbackTests, FillsSystemMemoryImageWithBlack) {
  std::array<std::uint8_t, 32> pixels;
  pixels.fill(0x7f);

  platf::img_t image;
  image.data = pixels.data();
  image.width = 4;
  image.height = 2;
  image.pixel_pitch = 4;
  image.row_pitch = 16;

  EXPECT_EQ(platf::fill_image_black(&image), 0);
  EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](std::uint8_t value) {
    return value == 0;
  }));
}

TEST(ImageFallbackTests, AcceptsGpuBackedImageWithoutCpuData) {
  platf::img_t image;
  image.width = 3840;
  image.height = 2160;
  image.pixel_pitch = 4;
  image.row_pitch = 15360;

  EXPECT_EQ(platf::fill_image_black(&image), 0);
}

TEST(ImageFallbackTests, RejectsInvalidImageMetadata) {
  platf::img_t image;
  image.width = -1;

  EXPECT_EQ(platf::fill_image_black(nullptr), -1);
  EXPECT_EQ(platf::fill_image_black(&image), -1);
}
