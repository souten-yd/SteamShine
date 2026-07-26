/**
 * @file src/platform/linux/gamescopegrab.cpp
 * @brief PipeWire capture provider for SteamShine-owned Gamescope sessions.
 */
#include "pipewire.cpp"
#include "src/config.h"
#include "src/logging.h"
#include "src/steamos_virtual_session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  /**
   * @brief Connected host PipeWire socket whose ownership can be transferred.
   */
  class pipewire_connection_t {
  public:
    /**
     * @brief Construct an empty connection.
     */
    pipewire_connection_t() = default;

    /**
     * @brief Prevent copying a unique socket descriptor.
     */
    pipewire_connection_t(const pipewire_connection_t &) = delete;

    /**
     * @brief Move a unique socket descriptor.
     *
     * @param other Connection from which ownership is moved.
     */
    pipewire_connection_t(pipewire_connection_t &&other) noexcept:
        fd_ {std::exchange(other.fd_, -1)} {}

    /**
     * @brief Release a locally owned socket descriptor.
     */
    ~pipewire_connection_t() {
      if (fd_ >= 0) {
        ::close(fd_);
      }
    }

    /**
     * @brief Connect to a current-user-owned PipeWire UNIX socket.
     *
     * @param runtime Host PipeWire runtime directory.
     * @param remote Host PipeWire remote name.
     * @param error Receives a non-secret diagnostic on failure.
     * @return Connected descriptor wrapper when validation and connect succeed.
     */
    static std::optional<pipewire_connection_t> connect(const std::filesystem::path &runtime, const std::string_view remote, std::string &error) {
      if (runtime.empty() || remote.empty() || remote.find('/') != std::string_view::npos) {
        error = "host_pipewire_socket_invalid";
        return std::nullopt;
      }
      const auto socket_path {runtime / remote};
      struct stat socket_stat {};
      if (::stat(socket_path.c_str(), &socket_stat) != 0 || !S_ISSOCK(socket_stat.st_mode) || socket_stat.st_uid != ::getuid()) {
        error = "host_pipewire_socket_missing";
        return std::nullopt;
      }
      if (socket_path.string().size() >= sizeof(sockaddr_un::sun_path)) {
        error = "host_pipewire_socket_path_too_long";
        return std::nullopt;
      }
      const int fd {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      if (fd < 0) {
        error = "host_pipewire_socket_open_failed";
        return std::nullopt;
      }
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
      const auto address_length {static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1)};
      if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), address_length) != 0) {
        ::close(fd);
        error = "host_pipewire_connect_failed";
        return std::nullopt;
      }
      pipewire_connection_t connection;
      connection.fd_ = fd;
      return connection;
    }

    /**
     * @brief Transfer the socket descriptor to PipeWire.
     *
     * @return Connected descriptor, or -1 when already transferred.
     */
    int release() {
      return std::exchange(fd_, -1);
    }

  private:
    int fd_ {-1};  ///< Locally owned connected PipeWire socket descriptor.
  };

  /**
   * @brief PipeWire node identity verified against an owned Gamescope process.
   */
  struct gamescope_node_t {
    uint32_t id {PW_ID_ANY};  ///< Volatile PipeWire global node ID.
    uint64_t object_serial {SPA_ID_INVALID};  ///< Stable PipeWire object serial.
    uint32_t client_id {PW_ID_ANY};  ///< PipeWire client that published this node.
    pid_t producer_pid {-1};  ///< Verified process ID of the publishing client.
  };

  /**
   * @brief PipeWire client identity used to authenticate a published node.
   */
  struct pipewire_client_t {
    uint32_t id {PW_ID_ANY};  ///< PipeWire global client ID.
    pid_t producer_pid {-1};  ///< Client process ID reported by PipeWire.
  };

  /**
   * @brief Parse a positive integer PipeWire property without accepting junk.
   *
   * @param value Property text to parse.
   * @return Parsed integer, or an empty optional for invalid input.
   */
  std::optional<unsigned long long> parse_property_integer(const char *value) {
    if (!value || !*value) {
      return std::nullopt;
    }
    char *end {};
    errno = 0;
    const auto parsed {std::strtoull(value, &end, 10)};
    if (errno != 0 || !end || *end != '\0') {
      return std::nullopt;
    }
    return parsed;
  }

  /**
   * @brief Read the first available property key from a PipeWire dictionary.
   *
   * @param properties PipeWire global properties.
   * @param keys Keys to check in priority order.
   * @return Matching property text, or null when no key is present.
   */
  const char *property(const spa_dict *properties, const std::initializer_list<const char *> keys) {
    for (const auto *key : keys) {
      if (const auto *value {spa_dict_lookup(properties, key)}) {
        return value;
      }
    }
    return nullptr;
  }

  /**
   * @brief Discover Gamescope Video/Source nodes from one host PipeWire core.
   */
  class gamescope_pipewire_registry_t {
  public:
    /**
     * @brief Stop PipeWire objects before destroying the event loop.
     */
    ~gamescope_pipewire_registry_t() {
      if (registry_) {
        spa_hook_remove(&registry_listener_);
      }
      if (core_) {
        pw_core_disconnect(core_);
      }
      if (context_) {
        pw_context_destroy(context_);
      }
      if (loop_) {
        pw_thread_loop_stop(loop_);
        pw_thread_loop_destroy(loop_);
      }
    }

    /**
     * @brief Attach a registry listener to a connected PipeWire socket.
     *
     * @param connection Connected host PipeWire socket.
     * @param error Receives a non-secret diagnostic on failure.
     * @return True when the registry listener is running.
     */
    bool connect(pipewire_connection_t &&connection, std::string &error) {
      loop_ = pw_thread_loop_new("SteamShine Gamescope registry", nullptr);
      if (!loop_ || pw_thread_loop_start(loop_) != 0) {
        error = "pipewire_registry_loop_failed";
        return false;
      }
      pw_thread_loop_lock(loop_);
      context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
      if (!context_) {
        pw_thread_loop_unlock(loop_);
        error = "pipewire_registry_context_failed";
        return false;
      }
      core_ = pw_context_connect_fd(context_, connection.release(), nullptr, 0);
      if (!core_) {
        pw_thread_loop_unlock(loop_);
        error = "pipewire_registry_connect_failed";
        return false;
      }
      registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
      if (!registry_) {
        pw_thread_loop_unlock(loop_);
        error = "pipewire_registry_unavailable";
        return false;
      }
      pw_registry_add_listener(registry_, &registry_listener_, &registry_events, this);
      pw_thread_loop_unlock(loop_);
      return true;
    }

    /**
     * @brief Wait for exactly one Video/Source node produced by Gamescope.
     *
     * @param gamescope_pid Owned Gamescope PID.
     * @param timeout Maximum discovery wait.
     * @param result Receives the verified candidate.
     * @param error Receives a failure reason.
     * @return True only when one current-core node matches PID and UID.
     */
    bool wait_for_owned_node(const pid_t gamescope_pid, const std::chrono::milliseconds timeout, gamescope_node_t &result, std::string &error) {
      const auto deadline {std::chrono::steady_clock::now() + timeout};
      while (std::chrono::steady_clock::now() < deadline) {
        {
          std::scoped_lock lock {mutex_};
          std::vector<gamescope_node_t> matches;
          for (const auto &candidate : candidates_) {
            const auto client {std::find_if(clients_.begin(), clients_.end(), [&candidate](const pipewire_client_t &identity) {
              return identity.id == candidate.client_id;
            })};
            if (client != clients_.end() && client->producer_pid == gamescope_pid) {
              matches.emplace_back(gamescope_node_t {candidate.id, candidate.object_serial, client->id, client->producer_pid});
            }
          }
          if (matches.size() == 1) {
            result = matches.front();
            return true;
          }
          if (matches.size() > 1) {
            error = "gamescope_node_ambiguous";
            return false;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {25});
      }
      error = "gamescope_node_not_published";
      return false;
    }

  private:
    /**
     * @brief Receive a PipeWire global and retain verified candidate properties.
     *
     * @param data Registry object.
     * @param id Global object ID.
     * @param permissions PipeWire permissions.
     * @param type Object interface type.
     * @param version Interface version.
     * @param properties Global properties.
     */
    static void on_global(void *data, const uint32_t id, uint32_t permissions [[maybe_unused]], const char *type, uint32_t version [[maybe_unused]], const spa_dict *properties) {
      if (!properties) {
        return;
      }
      auto *self {static_cast<gamescope_pipewire_registry_t *>(data)};
      if (std::string_view {type} == PW_TYPE_INTERFACE_Client) {
        const auto process_id {parse_property_integer(property(properties, {"application.process.id", "pipewire.sec.pid", "process.id"}))};
        const auto process_user {parse_property_integer(property(properties, {"application.process.user", "pipewire.sec.uid", "process.user"}))};
        if (!process_id || !process_user || *process_user != static_cast<unsigned long long>(::getuid())) {
          return;
        }
        std::scoped_lock lock {self->mutex_};
        std::erase_if(self->clients_, [id](const pipewire_client_t &client) {
          return client.id == id;
        });
        self->clients_.push_back({id, static_cast<pid_t>(*process_id)});
        return;
      }
      if (std::string_view {type} != PW_TYPE_INTERFACE_Node || std::string_view {property(properties, {PW_KEY_MEDIA_CLASS}) ?: ""} != "Video/Source") {
        return;
      }
      const auto client_id {parse_property_integer(property(properties, {"client.id"}))};
      const auto object_serial {parse_property_integer(property(properties, {PW_KEY_OBJECT_SERIAL}))};
      if (!client_id || !object_serial || *client_id > UINT32_MAX) {
        return;
      }
      std::scoped_lock lock {self->mutex_};
      self->candidates_.erase(std::remove_if(self->candidates_.begin(), self->candidates_.end(), [id](const gamescope_node_t &candidate) {
                                return candidate.id == id;
                              }),
                              self->candidates_.end());
      self->candidates_.push_back({id, *object_serial, static_cast<uint32_t>(*client_id), -1});
      BOOST_LOG(debug) << "PIPEWIRE_NODE_DISCOVERED id=" << id << " serial=" << *object_serial << " client_id=" << *client_id << " media_class=Video/Source";
    }

    /**
     * @brief Remove a disappearing PipeWire candidate node.
     *
     * @param data Registry object.
     * @param id Removed PipeWire global ID.
     */
    static void on_global_remove(void *data, const uint32_t id) {
      auto *self {static_cast<gamescope_pipewire_registry_t *>(data)};
      std::scoped_lock lock {self->mutex_};
      std::erase_if(self->candidates_, [id](const gamescope_node_t &candidate) {
        return candidate.id == id;
      });
      std::erase_if(self->clients_, [id](const pipewire_client_t &client) {
        return client.id == id;
      });
    }

    static constexpr pw_registry_events registry_events {
      .version = PW_VERSION_REGISTRY_EVENTS,
      .global = on_global,
      .global_remove = on_global_remove,
    };  ///< Events used to monitor candidate PipeWire nodes.

    pw_thread_loop *loop_ {};  ///< Registry event loop.
    pw_context *context_ {};  ///< Registry PipeWire context.
    pw_core *core_ {};  ///< Connected host PipeWire core.
    pw_registry *registry_ {};  ///< Host PipeWire registry.
    spa_hook registry_listener_ {};  ///< Registry event listener hook.
    std::mutex mutex_;  ///< Synchronizes candidates with PipeWire callbacks.
    std::vector<gamescope_node_t> candidates_;  ///< Current Video/Source candidates keyed by PipeWire client ID.
    std::vector<pipewire_client_t> clients_;  ///< Current user-owned PipeWire client identities.
  };
}  // namespace

namespace gamescope_pipewire {
  /**
   * @brief Direct Gamescope PipeWire capture display.
   */
  class display_t: public pipewire::pipewire_display_t {
  public:
    /**
     * @brief Discover the owned Gamescope node and configure the shared consumer.
     *
     * @param display_name Ignored synthetic Gamescope display name.
     * @param out_pipewire_fd Receives a direct host PipeWire descriptor.
     * @param out_pipewire_node Receives the owned Gamescope node ID.
     * @param out_pipewire_objectserial Receives the owned Gamescope object serial.
     * @return Zero when the owned node was verified and configured.
     */
    int configure_stream(const std::string &display_name [[maybe_unused]], int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      std::string runtime;
      std::string remote;
      int gamescope_pid {};
      if (!steamos_virtual_session::gamescope_pipewire_endpoint(runtime, remote, gamescope_pid)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=owned_session_endpoint_unavailable";
        return -1;
      }
      std::string failure;
      auto discovery_connection {pipewire_connection_t::connect(runtime, remote, failure)};
      if (!discovery_connection) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=" << failure;
        return -1;
      }
      gamescope_pipewire_registry_t registry;
      if (!registry.connect(std::move(*discovery_connection), failure)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=" << failure;
        return -1;
      }
      gamescope_node_t node;
      if (!registry.wait_for_owned_node(gamescope_pid, std::chrono::milliseconds {config::steamos_virtual_display.pipewire_node_timeout_milliseconds}, node, failure)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=" << failure << " gamescope_pid=" << gamescope_pid;
        return -1;
      }
      auto stream_connection {pipewire_connection_t::connect(runtime, remote, failure)};
      if (!stream_connection) {
        BOOST_LOG(error) << "PIPEWIRE_STREAM_CONNECT_FAILED reason=" << failure;
        return -1;
      }
      const auto session {steamos_virtual_session::status_snapshot()};
      if (session.width <= 0 || session.height <= 0 || session.fps <= 0) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=invalid_owned_session_dimensions";
        return -1;
      }
      width = session.width;
      height = session.height;
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
      offset_x = 0;
      offset_y = 0;
      out_pipewire_fd = stream_connection->release();
      out_pipewire_node = node.id;
      out_pipewire_objectserial = node.object_serial;
      steamos_virtual_session::mark_gamescope_pipewire_node(node.id, node.object_serial, node.producer_pid);
      BOOST_LOG(info) << "CAPTURE_SOURCE source=gamescope_pipewire PIPEWIRE_NODE id=" << node.id << " PIPEWIRE_SERIAL serial=" << node.object_serial << " PRODUCER_PID=" << node.producer_pid << " DRM_RENDER_NODE=" << session.render_node;
      return 0;
    }

    /**
     * @brief Keep virtual-display dimensions rather than querying desktop Wayland outputs.
     */
    void verify_and_update_display_parameters() override {}

    /**
     * @brief Fail the owned virtual session when its verified PipeWire node disappears.
     *
     * @param out_status Receives the terminal capture status.
     * @return True because the generic PipeWire reconnect path must not select a desktop source.
     */
    bool check_stream_dead(platf::capture_e &out_status) override {
      BOOST_LOG(error) << "PIPEWIRE_NODE_DISAPPEARED source=gamescope_pipewire";
      steamos_virtual_session::mark_capture_lost();
      out_status = platf::capture_e::error;
      return true;
    }
  };
}  // namespace gamescope_pipewire

namespace platf {
  /**
   * @brief Create capture for a SteamShine-owned Gamescope PipeWire source.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Synthetic owned Gamescope display name.
   * @param config Capture configuration.
   * @return PipeWire display, or nullptr when the owned node cannot be verified.
   */
  std::shared_ptr<display_t> gamescope_pipewire_display(const mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[gamescope-pipewire] Unsupported hardware device type";
      return nullptr;
    }
    auto display {std::make_shared<gamescope_pipewire::display_t>()};
    if (display->init(hwdevice_type, display_name, config) != 0) {
      steamos_virtual_session::mark_capture_lost();
      return nullptr;
    }
    steamos_virtual_session::mark_capture_ready();
    return display;
  }
}  // namespace platf
