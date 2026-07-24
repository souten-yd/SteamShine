/**
 * @file src/platform/linux/vulkan_encode.h
 * @brief Declarations for FFmpeg Vulkan Video encoder.
 */
#pragma once

#include "src/platform/common.h"

extern "C" struct AVBufferRef;

namespace vk {

  /**
   * @brief Initialize Vulkan hardware device for FFmpeg encoding.
   * @param encode_device The encode device (vk_t).
   * @param hw_device_buf Output hardware device buffer.
   * @return 0 on success, negative on error.
   */
  int vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  /**
   * @brief Create a Vulkan encode device for RAM capture.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @return Constructed AVCodec encode device ram object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_ram(int width, int height);

  /**
   * @brief Create a Vulkan encode device for VRAM capture.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param offset_x Offset x.
   * @param offset_y Offset y.
   * @return Constructed AVCodec encode device VRAM object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_vram(int width, int height, int offset_x, int offset_y);

  /**
   * @brief Check if FFmpeg Vulkan Video encoding is available.
   *
   * @return True when FFmpeg Vulkan Video encoding is available.
   */
  bool validate();

  /**
   * @brief Open the selected Vulkan Video H.264 encoder without streaming.
   *
   * This explicit hardware-test preflight uses the same FFmpeg Vulkan device
   * selection as the streaming path. It validates device creation, the
   * H.264 Vulkan encoder, and codec-context initialization without writing a
   * bitstream or adding work to a streaming thread.
   *
   * @param error Receives a diagnostic reason when the preflight fails.
   * @return True when the selected GPU accepts an H.264 Vulkan encoder context.
   */
  bool probe_h264(std::string &error);

}  // namespace vk
