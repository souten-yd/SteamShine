/**
 * @file src/platform/linux/gamescope_presenter_pipewire.cpp
 * @brief Independent DMA-BUF-only PipeWire consumer for local presentation.
 */
#include "gamescope_presenter.h"
#include "src/logging.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <libdrm/drm_fourcc.h>
#include <memory>
#include <mutex>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <unistd.h>
#include <utility>

namespace gamescope_presenter {
  namespace {
    constexpr size_t pod_buffer_size {1024};  ///< Maximum temporary SPA pod size for one format request.

    /**
     * @brief Convert supported SPA formats to DRM fourcc values.
     *
     * @param format Negotiated SPA raw-video format.
     * @return DRM fourcc, or zero when no direct DMA-BUF import is supported.
     */
    uint32_t drm_fourcc_for_format(const enum spa_video_format format) {
      switch (format) {
        case SPA_VIDEO_FORMAT_BGRx:
          return DRM_FORMAT_XRGB8888;
        case SPA_VIDEO_FORMAT_BGRA:
          return DRM_FORMAT_ARGB8888;
        case SPA_VIDEO_FORMAT_xBGR_210LE:
          return DRM_FORMAT_XBGR2101010;
        case SPA_VIDEO_FORMAT_ARGB_210LE:
          return DRM_FORMAT_BGRA1010102;
        default:
          return 0;
      }
    }

    /**
     * @brief Shared PipeWire state retained by frames until their release.
     */
    struct connection_t {
      std::mutex lifecycle_mutex;  ///< Prevents buffer returns from racing PipeWire-loop destruction.
      std::mutex mutex;  ///< Serializes stream destruction and buffer returns.
      struct pw_thread_loop *loop {nullptr};  ///< Dedicated PipeWire thread loop.
      struct pw_context *context {nullptr};  ///< PipeWire context for this consumer.
      struct pw_core *core {nullptr};  ///< Dedicated PipeWire core connection.
      struct pw_stream *stream {nullptr};  ///< DMA-BUF-only input stream.
      struct spa_hook listener {};  ///< Registered stream callback hook.
      dma_buf_frame_callback_t callback;  ///< Renderer-facing frame handoff.
      std::atomic<bool> active {false};  ///< Whether buffers can still be returned.
      std::atomic<bool> dma_buf_negotiated {false};  ///< Whether the server negotiated DMA-BUF.
      std::atomic<uint64_t> sequence {0};  ///< Local monotonically increasing frame counter.
      uint32_t fourcc {0};  ///< Negotiated DRM fourcc.
      uint64_t modifier {0};  ///< Negotiated DRM modifier.
      uint32_t width {0};  ///< Negotiated frame width.
      uint32_t height {0};  ///< Negotiated frame height.
      std::weak_ptr<connection_t> self;  ///< Shared ownership used by outstanding frame releases.

      /**
       * @brief Return one held source buffer without blocking the remote consumer.
       *
       * @param buffer PipeWire buffer acquired by this consumer.
       */
      void release_buffer(struct pw_buffer *buffer) {
        std::scoped_lock lifecycle_lock {lifecycle_mutex};
        if (!buffer || !loop || !active.load(std::memory_order_acquire)) {
          return;
        }
        const bool lock_needed {!pw_thread_loop_in_thread(loop)};
        if (lock_needed) {
          pw_thread_loop_lock(loop);
        }
        {
          std::scoped_lock lock {mutex};
          if (active.load(std::memory_order_acquire) && stream) {
            pw_stream_queue_buffer(stream, buffer);
          }
        }
        if (lock_needed) {
          pw_thread_loop_unlock(loop);
        }
      }
    };

    /**
     * @brief Request DMA-BUF buffers after PipeWire reports its raw format.
     *
     * @param connection Consumer callback state.
     */
    void request_dma_buf_buffers(connection_t &connection) {
      std::array<uint8_t, pod_buffer_size> buffer;
      struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
      const struct spa_pod *parameter {static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_DmaBuf)))};
      pw_stream_update_params(connection.stream, &parameter, 1);
    }

    /**
     * @brief Handle negotiated PipeWire raw-video format changes.
     *
     * @param user_data Connection callback state.
     * @param id Changed PipeWire parameter ID.
     * @param parameter New parameter, or null.
     */
    void on_param_changed(void *user_data, const uint32_t id, const struct spa_pod *parameter) {
      auto &connection {*static_cast<connection_t *>(user_data)};
      connection.dma_buf_negotiated.store(false, std::memory_order_release);
      if (!parameter || id != SPA_PARAM_Format) {
        return;
      }
      struct spa_video_info format {};
      if (spa_format_parse(parameter, &format.media_type, &format.media_subtype) < 0 || format.media_type != SPA_MEDIA_TYPE_video || format.media_subtype != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(parameter, &format.info.raw) < 0) {
        return;
      }
      const auto fourcc {drm_fourcc_for_format(format.info.raw.format)};
      const auto *const modifier_property {spa_pod_find_prop(parameter, nullptr, SPA_FORMAT_VIDEO_modifier)};
      if (!fourcc || !modifier_property) {
        BOOST_LOG(error) << "LOCAL_PRESENTER_STOPPED reason=non_dmabuf_format";
        return;
      }
      connection.fourcc = fourcc;
      connection.modifier = format.info.raw.modifier;
      connection.width = format.info.raw.size.width;
      connection.height = format.info.raw.size.height;
      connection.dma_buf_negotiated.store(true, std::memory_order_release);
      request_dma_buf_buffers(connection);
    }

    /**
     * @brief Drop or hand off the newest DMA-BUF without CPU access.
     *
     * @param user_data Connection callback state.
     */
    void on_process(void *user_data) {
      auto &connection {*static_cast<connection_t *>(user_data)};
      struct pw_buffer *newest {nullptr};
      while (auto *candidate = pw_stream_dequeue_buffer(connection.stream)) {
        if (newest) {
          pw_stream_queue_buffer(connection.stream, newest);
        }
        newest = candidate;
      }
      if (!newest) {
        return;
      }
      auto *const buffer {newest->buffer};
      if (!connection.dma_buf_negotiated.load(std::memory_order_acquire) || !buffer || buffer->n_datas == 0 || buffer->datas[0].type != SPA_DATA_DmaBuf) {
        pw_stream_queue_buffer(connection.stream, newest);
        return;
      }
      dma_buf_frame_t frame {
        .sequence = connection.sequence.fetch_add(1, std::memory_order_relaxed) + 1,
        .width = connection.width,
        .height = connection.height,
        .fourcc = connection.fourcc,
        .modifier = connection.modifier,
      };
      for (uint32_t index {}; index < std::min<uint32_t>(buffer->n_datas, frame.fds.size()); ++index) {
        frame.fds[index] = buffer->datas[index].fd;
        frame.pitches[index] = buffer->datas[index].chunk ? buffer->datas[index].chunk->stride : 0;
        frame.offsets[index] = buffer->datas[index].chunk ? buffer->datas[index].chunk->offset : 0;
      }
      const auto retained_connection {connection.self.lock()};
      frame.release = [retained_connection, newest]() {
        if (retained_connection) {
          retained_connection->release_buffer(newest);
        }
      };
      if (connection.callback) {
        connection.callback(std::move(frame));
      } else {
        frame.release();
      }
    }

    constexpr struct pw_stream_events stream_events {
      .version = PW_VERSION_STREAM_EVENTS,
      .param_changed = on_param_changed,
      .process = on_process,
    };
  }  // namespace

  /**
   * @brief Private ownership shared between the presenter and frame releases.
   */
  struct local_pipewire_consumer_t::impl_t {
    std::shared_ptr<connection_t> connection;  ///< State retained while a frame owns a PipeWire buffer.
  };

  local_pipewire_consumer_t::local_pipewire_consumer_t():
      impl_ {std::make_unique<impl_t>()} {}

  local_pipewire_consumer_t::~local_pipewire_consumer_t() {
    stop();
  }

  bool local_pipewire_consumer_t::start(pipewire_capture::stream_descriptor_t descriptor, dma_buf_frame_callback_t callback, std::string &error) {
    stop();
    if (!pipewire_capture::has_verified_source_identity(descriptor) || descriptor.connected_core_fd < 0) {
      if (descriptor.connected_core_fd >= 0) {
        close(descriptor.connected_core_fd);
      }
      error = "Local presenter requires a verified dedicated PipeWire descriptor";
      return false;
    }
    pw_init(nullptr, nullptr);
    auto connection {std::make_shared<connection_t>()};
    connection->self = connection;
    connection->callback = std::move(callback);
    connection->loop = pw_thread_loop_new("SteamShine local presenter", nullptr);
    if (!connection->loop || pw_thread_loop_start(connection->loop) < 0) {
      if (connection->loop) {
        pw_thread_loop_destroy(connection->loop);
      }
      close(descriptor.connected_core_fd);
      error = "Failed to start the local-presenter PipeWire loop";
      return false;
    }
    impl_->connection = connection;
    pw_thread_loop_lock(connection->loop);
    connection->context = pw_context_new(pw_thread_loop_get_loop(connection->loop), nullptr, 0);
    if (connection->context) {
      connection->core = pw_context_connect_fd(connection->context, descriptor.connected_core_fd, nullptr, 0);
      descriptor.connected_core_fd = -1;
    }
    if (!connection->core) {
      pw_thread_loop_unlock(connection->loop);
      stop();
      error = "Failed to connect the local-presenter PipeWire core";
      return false;
    }
    auto *const properties {pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr)};
    pw_properties_setf(properties, PW_KEY_TARGET_OBJECT, "%" PRIu64, descriptor.object_serial);
    connection->stream = pw_stream_new(connection->core, "SteamShine Local Presenter", properties);
    if (!connection->stream) {
      pw_thread_loop_unlock(connection->loop);
      stop();
      error = "Failed to create the local-presenter PipeWire stream";
      return false;
    }
    pw_stream_add_listener(connection->stream, &connection->listener, &stream_events, connection.get());
    std::array<uint8_t, pod_buffer_size> buffer;
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
    const struct spa_pod *format {static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx)))};
    const int result {pw_stream_connect(connection->stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT, &format, 1)};
    if (result < 0) {
      pw_thread_loop_unlock(connection->loop);
      stop();
      error = "Failed to connect the local-presenter PipeWire stream";
      return false;
    }
    connection->active.store(true, std::memory_order_release);
    pw_thread_loop_unlock(connection->loop);
    BOOST_LOG(info) << "LOCAL_PRESENTER_PIPEWIRE_STARTED node_id=" << descriptor.node_id << " object_serial=" << descriptor.object_serial << " producer_pid=" << descriptor.producer_pid << " render_node=" << descriptor.render_node;
    return true;
  }

  void local_pipewire_consumer_t::stop() {
    const auto connection {std::exchange(impl_->connection, {})};
    if (!connection || !connection->loop) {
      return;
    }
    std::scoped_lock lifecycle_lock {connection->lifecycle_mutex};
    pw_thread_loop_lock(connection->loop);
    connection->active.store(false, std::memory_order_release);
    {
      std::scoped_lock lock {connection->mutex};
      if (connection->stream) {
        pw_stream_disconnect(connection->stream);
        pw_stream_destroy(connection->stream);
        connection->stream = nullptr;
      }
      if (connection->core) {
        pw_core_disconnect(connection->core);
        connection->core = nullptr;
      }
      if (connection->context) {
        pw_context_destroy(connection->context);
        connection->context = nullptr;
      }
    }
    pw_thread_loop_unlock(connection->loop);
    pw_thread_loop_stop(connection->loop);
    pw_thread_loop_destroy(connection->loop);
    connection->loop = nullptr;
    BOOST_LOG(info) << "LOCAL_PRESENTER_PIPEWIRE_STOPPED";
  }

  bool local_pipewire_consumer_t::active() const {
    return impl_->connection && impl_->connection->active.load(std::memory_order_acquire);
  }
}  // namespace gamescope_presenter
