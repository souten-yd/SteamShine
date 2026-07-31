/**
 * @file src/platform/linux/gamescope_source.cpp
 * @brief Verified Gamescope PipeWire source selection implementation.
 */
#include "gamescope_source.h"

#include "pipewire_capture.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__linux__)
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

#if defined(SUNSHINE_BUILD_PIPEWIRE)
  #include <pipewire/pipewire.h>
#endif

namespace gamescope_source {
  namespace {
    /**
     * @brief Read the Linux kernel process start-time field from a stat record.
     *
     * @param contents Complete `/proc/<pid>/stat` contents.
     * @return Field 22, or std::nullopt for malformed records.
     */
    std::optional<uint64_t> process_start_time_from_stat(const std::string &contents) {
      const auto command_end {contents.rfind(')')};
      if (command_end == std::string::npos || command_end + 2 >= contents.size()) {
        return std::nullopt;
      }
      std::istringstream fields {contents.substr(command_end + 2)};
      std::string field;
      for (int index {0}; index < 20; ++index) {
        if (!(fields >> field)) {
          return std::nullopt;
        }
      }
      try {
        size_t consumed {};
        const auto value {std::stoull(field, &consumed)};
        return consumed == field.size() ? std::optional<uint64_t> {value} : std::nullopt;
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }

    /**
     * @brief Verify the command-line evidence for a Gamescope process.
     *
     * SteamOS installs Gamescope with a file capability. Linux then prevents
     * ordinary same-user processes from reading `/proc/<pid>/exe`, even though
     * the process is a direct SteamShine child. In that case the kernel-owned
     * process name and nul-separated command line provide the available
     * identity evidence without weakening the PID start-time or UID checks.
     *
     * @param process_directory Procfs directory for the candidate process.
     * @return A stable Gamescope marker when both procfs fields agree.
     */
    std::optional<std::filesystem::path> gamescope_identity_from_command_line(const std::filesystem::path &process_directory) {
      std::ifstream command_file {process_directory / "cmdline", std::ios::binary};
      const std::string command {std::istreambuf_iterator<char> {command_file}, {}};
      std::ifstream comm_file {process_directory / "comm"};
      std::string comm;
      std::getline(comm_file, comm);
      return has_gamescope_command_identity(command, comm) ? std::optional<std::filesystem::path> {"gamescope"} : std::nullopt;
    }

    /**
     * @brief Check whether a source meets universal identity and GPU constraints.
     *
     * @param source Candidate source to inspect.
     * @param request Source-selection constraints.
     * @return True only when the candidate is safe to consider further.
     */
    bool eligible(const gamescope_source_t &source, const source_selection_request_t &request) {
      return source.identity_verified && is_gamescope_capture_media_class(source.media_class) && source.producer_pid > 0 && source.producer_start_time != 0 && !source.executable.empty() &&
             (request.required_render_node.empty() || pipewire_capture::matches_selected_render_node(source.render_node, request.required_render_node));
    }

    /**
     * @brief Find one unique source matching an origin and optional explicit PID.
     *
     * @param sources Candidate source list.
     * @param request Source-selection constraints.
     * @param origin Required ownership origin.
     * @return The unique source, or a fail-closed selection error.
     */
    std::expected<gamescope_source_t, source_error_e> select_unique(const std::vector<gamescope_source_t> &sources, const source_selection_request_t &request, const steamos_virtual_session::session_origin_e origin) {
      std::vector<const gamescope_source_t *> matches;
      for (const auto &source : sources) {
        if (!eligible(source, request) || source.origin != origin) {
          continue;
        }
        if (request.explicit_gamescope_pid && source.producer_pid != *request.explicit_gamescope_pid) {
          continue;
        }
        if (origin == steamos_virtual_session::session_origin_e::attached_existing && !source.game_mode_verified) {
          continue;
        }
        matches.push_back(&source);
      }
      if (matches.empty()) {
        return std::unexpected {request.explicit_gamescope_pid ? source_error_e::explicit_pid_invalid : source_error_e::unavailable};
      }
      if (matches.size() != 1) {
        return std::unexpected {source_error_e::ambiguous};
      }
      return *matches.front();
    }

#if defined(SUNSHINE_BUILD_PIPEWIRE)
    /**
     * @brief Own one connected PipeWire UNIX socket until the core consumes it.
     */
    class pipewire_connection_t {
    public:
      /**
       * @brief Connect to a user-owned host PipeWire socket.
       *
       * @param runtime_directory Host runtime directory.
       * @param remote_name Socket name below the runtime directory.
       * @param error Receives a stable failure reason.
       * @return Socket owner when validation and connect succeed.
       */
      static std::optional<pipewire_connection_t> connect(const std::string &runtime_directory, const std::string_view remote_name, std::string &error) {
        if (runtime_directory.empty() || remote_name.empty() || remote_name.find('/') != std::string_view::npos) {
          error = "gamescope_source_pipewire_socket_invalid";
          return std::nullopt;
        }
        const std::filesystem::path socket_path {std::filesystem::path {runtime_directory} / remote_name};
        struct stat socket_stat {};
        if (::stat(socket_path.c_str(), &socket_stat) != 0 || !S_ISSOCK(socket_stat.st_mode) || socket_stat.st_uid != ::getuid()) {
          error = "gamescope_source_pipewire_socket_missing";
          return std::nullopt;
        }
        if (socket_path.string().size() >= sizeof(sockaddr_un::sun_path)) {
          error = "gamescope_source_pipewire_socket_path_too_long";
          return std::nullopt;
        }
        const int socket {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (socket < 0) {
          error = "gamescope_source_pipewire_socket_open_failed";
          return std::nullopt;
        }
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
        const auto address_length {static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1)};
        if (::connect(socket, reinterpret_cast<const sockaddr *>(&address), address_length) != 0) {
          ::close(socket);
          error = "gamescope_source_pipewire_connect_failed";
          return std::nullopt;
        }
        return pipewire_connection_t {socket};
      }

      /**
       * @brief Construct an owner for a connected descriptor.
       *
       * @param socket Connected UNIX descriptor.
       */
      explicit pipewire_connection_t(const int socket):
          socket_ {socket} {}

      /**
       * @brief Prevent accidental copying of a unique descriptor.
       */
      pipewire_connection_t(const pipewire_connection_t &) = delete;

      /**
       * @brief Move a unique descriptor owner.
       *
       * @param other Owner from which to transfer the descriptor.
       */
      pipewire_connection_t(pipewire_connection_t &&other) noexcept:
          socket_ {std::exchange(other.socket_, -1)} {}

      /**
       * @brief Close a descriptor not consumed by PipeWire.
       */
      ~pipewire_connection_t() {
        if (socket_ >= 0) {
          ::close(socket_);
        }
      }

      /**
       * @brief Transfer descriptor ownership to PipeWire.
       *
       * @return Connected descriptor, or -1 after ownership transfer.
       */
      int release() {
        return std::exchange(socket_, -1);
      }

    private:
      int socket_ {-1};  ///< Locally owned connected PipeWire descriptor.
    };

    /**
     * @brief Retained PipeWire Client identity needed to validate a source node.
     */
    struct pipewire_client_t {
      uint32_t id {PW_ID_ANY};  ///< PipeWire client global ID.
      int pid {-1};  ///< Client-reported process ID.
      int uid {-1};  ///< Client-reported user ID.
    };

    /**
     * @brief Read one PipeWire property from a preferred key list.
     *
     * @param properties PipeWire global properties.
     * @param keys Keys to inspect in priority order.
     * @return First present value, or null when absent.
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
     * @brief Parse a positive integer PipeWire property without accepting junk.
     *
     * @param value Property text to parse.
     * @return Parsed value, or std::nullopt for invalid input.
     */
    std::optional<unsigned long long> parse_property_integer(const char *value) {
      if (!value || !*value) {
        return std::nullopt;
      }
      char *end {};
      errno = 0;
      const auto parsed {std::strtoull(value, &end, 10)};
      return errno == 0 && end && *end == '\0' ? std::optional<unsigned long long> {parsed} : std::nullopt;
    }

    /**
     * @brief Parse the first valid integer from an ordered PipeWire property list.
     *
     * PipeWire commonly exposes both a human-readable user name and a numeric
     * security UID. A present but nonnumeric preferred property must therefore
     * not prevent use of a later numeric identity property.
     *
     * @param properties PipeWire global properties.
     * @param keys Keys to inspect in priority order.
     * @return First valid integer property, or std::nullopt when none is valid.
     */
    std::optional<unsigned long long> integer_property(const spa_dict *properties, const std::initializer_list<const char *> keys) {
      for (const auto *key : keys) {
        if (const auto parsed {parse_property_integer(spa_dict_lookup(properties, key))}) {
          return parsed;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Check strict Game Mode evidence without using a process name alone.
     *
     * @param identity Verified Gamescope process identity.
     * @return True only when command-line and cgroup metadata identify a SteamOS session.
     */
    bool is_game_mode_gamescope(const process_identity_t &identity) {
      std::ifstream command_line {"/proc/" + std::to_string(identity.pid) + "/cmdline", std::ios::binary};
      const std::string command {std::istreambuf_iterator<char> {command_line}, {}};
      std::ifstream cgroup_file {"/proc/" + std::to_string(identity.pid) + "/cgroup"};
      const std::string cgroup {std::istreambuf_iterator<char> {cgroup_file}, {}};
      return has_game_mode_session_identity(command, cgroup);
    }

    /**
     * @brief Registry snapshot that joins capture-output nodes to PipeWire clients.
     */
    class pipewire_registry_t {
    public:
      /**
       * @brief Destroy PipeWire objects before their event loop.
       */
      ~pipewire_registry_t() {
        if (registry_) {
          spa_hook_remove(&listener_);
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
       * @brief Connect a registry listener to the host PipeWire core.
       *
       * @param connection Connected PipeWire socket owner.
       * @param error Receives a stable failure reason.
       * @return True after the registry listener starts.
       */
      bool connect(pipewire_connection_t &&connection, std::string &error) {
        loop_ = pw_thread_loop_new("SteamShine Gamescope source registry", nullptr);
        if (!loop_ || pw_thread_loop_start(loop_) != 0) {
          error = "gamescope_source_registry_loop_failed";
          return false;
        }
        pw_thread_loop_lock(loop_);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        core_ = context_ ? pw_context_connect_fd(context_, connection.release(), nullptr, 0) : nullptr;
        registry_ = core_ ? pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0) : nullptr;
        if (!registry_) {
          pw_thread_loop_unlock(loop_);
          error = "gamescope_source_registry_connect_failed";
          return false;
        }
        pw_registry_add_listener(registry_, &listener_, &events, this);
        pw_thread_loop_unlock(loop_);
        return true;
      }

      /**
       * @brief Return verified Gamescope nodes collected during a bounded wait.
       *
       * @param timeout Maximum discovery wait.
       * @return Verified descriptor list.
       */
      std::vector<gamescope_source_t> snapshot(const std::chrono::milliseconds timeout) {
        const auto deadline {std::chrono::steady_clock::now() + timeout};
        while (std::chrono::steady_clock::now() < deadline) {
          {
            std::scoped_lock lock {mutex_};
            if (has_joined_gamescope_source()) {
              break;
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds {25});
        }
        std::scoped_lock lock {mutex_};
        std::vector<gamescope_source_t> result;
        for (auto source : sources_) {
          const auto client {std::find_if(clients_.begin(), clients_.end(), [&source](const pipewire_client_t &candidate) {
            return candidate.id == source.client_id;
          })};
          if (client == clients_.end() || client->uid != static_cast<int>(::getuid())) {
            continue;
          }
          const auto identity {read_process_identity(client->pid)};
          if (!identity || identity->uid != client->uid || identity->executable.filename() != "gamescope") {
            continue;
          }
          source.producer_pid = identity->pid;
          source.producer_uid = identity->uid;
          source.producer_start_time = identity->start_time;
          source.executable = identity->executable.string();
          source.identity_verified = true;
          source.game_mode_verified = is_game_mode_gamescope(*identity);
          source.origin = steamos_virtual_session::session_origin_e::attached_existing;
          result.emplace_back(std::move(source));
        }
        return result;
      }

    private:
      /**
       * @brief Check whether registry data contains a joined Gamescope source.
       *
       * The PipeWire registry may announce unrelated clients and sources before
       * Gamescope publishes its capture node. Waiting for a joined Gamescope
       * pair prevents those globals from ending discovery prematurely.
       *
       * @return True when a capture-output node is joined to a live Gamescope client.
       */
      bool has_joined_gamescope_source() const {
        return std::any_of(sources_.begin(), sources_.end(), [this](const gamescope_source_t &source) {
          const auto client {std::find_if(clients_.begin(), clients_.end(), [&source](const pipewire_client_t &candidate) {
            return candidate.id == source.client_id && candidate.uid == static_cast<int>(::getuid());
          })};
          const auto identity {client == clients_.end() ? std::nullopt : read_process_identity(client->pid)};
          return identity && identity->uid == client->uid && identity->executable.filename() == "gamescope";
        });
      }

      /**
       * @brief Handle one PipeWire registry global announcement.
       *
       * @param data Registry receiver.
       * @param id Global object ID.
       * @param permissions PipeWire permissions.
       * @param type Interface type.
       * @param version Interface version.
       * @param properties Global properties.
       */
      static void on_global(void *data, const uint32_t id, uint32_t permissions [[maybe_unused]], const char *type, uint32_t version [[maybe_unused]], const spa_dict *properties) {
        if (!properties) {
          return;
        }
        auto *self {static_cast<pipewire_registry_t *>(data)};
        if (std::string_view {type} == PW_TYPE_INTERFACE_Client) {
          const auto pid {integer_property(properties, {"application.process.id", "pipewire.sec.pid", "process.id"})};
          const auto uid {integer_property(properties, {"application.process.user", "pipewire.sec.uid", "process.user"})};
          if (!pid || !uid || *pid > INT_MAX || *uid > INT_MAX) {
            return;
          }
          std::scoped_lock lock {self->mutex_};
          std::erase_if(self->clients_, [id](const pipewire_client_t &client) {
            return client.id == id;
          });
          self->clients_.push_back({id, static_cast<int>(*pid), static_cast<int>(*uid)});
          return;
        }
        if (std::string_view {type} != PW_TYPE_INTERFACE_Node || !is_gamescope_capture_media_class(property(properties, {PW_KEY_MEDIA_CLASS}) ?: "")) {
          return;
        }
        const auto client_id {parse_property_integer(property(properties, {"client.id"}))};
        const auto object_serial {parse_property_integer(property(properties, {PW_KEY_OBJECT_SERIAL}))};
        if (!client_id || !object_serial || *client_id > UINT32_MAX) {
          return;
        }
        gamescope_source_t source;
        source.node_id = id;
        source.object_serial = *object_serial;
        source.client_id = static_cast<uint32_t>(*client_id);
        source.node_name = property(properties, {PW_KEY_NODE_NAME}) ?: "";
        source.node_description = property(properties, {PW_KEY_NODE_DESCRIPTION}) ?: "";
        source.application_name = property(properties, {"application.name"}) ?: "";
        source.media_class = property(properties, {PW_KEY_MEDIA_CLASS}) ?: "";
        source.render_node = property(properties, {"api.vulkan.render-node", "render.node", "device.path"}) ?: "";
        std::scoped_lock lock {self->mutex_};
        std::erase_if(self->sources_, [id](const gamescope_source_t &candidate) {
          return candidate.node_id == id;
        });
        self->sources_.emplace_back(std::move(source));
      }

      /**
       * @brief Drop removed PipeWire client or node metadata.
       *
       * @param data Registry receiver.
       * @param id Removed global object ID.
       */
      static void on_global_remove(void *data, const uint32_t id) {
        auto *self {static_cast<pipewire_registry_t *>(data)};
        std::scoped_lock lock {self->mutex_};
        std::erase_if(self->clients_, [id](const pipewire_client_t &client) {
          return client.id == id;
        });
        std::erase_if(self->sources_, [id](const gamescope_source_t &source) {
          return source.node_id == id;
        });
      }

      static constexpr pw_registry_events events {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = on_global,
        .global_remove = on_global_remove,
      };  ///< Registry callbacks for source discovery.

      pw_thread_loop *loop_ {};  ///< PipeWire registry event loop.
      pw_context *context_ {};  ///< Connected PipeWire context.
      pw_core *core_ {};  ///< Connected PipeWire core.
      pw_registry *registry_ {};  ///< PipeWire registry listener.
      spa_hook listener_ {};  ///< Registry callback hook.
      std::mutex mutex_;  ///< Synchronizes registry callback state.
      std::vector<pipewire_client_t> clients_;  ///< Current PipeWire client identities.
      std::vector<gamescope_source_t> sources_;  ///< Current capture-output node descriptors.
    };
#endif
  }  // namespace

  std::optional<process_identity_t> read_process_identity(const int pid) {
#if defined(__linux__)
    if (pid <= 0) {
      return std::nullopt;
    }
    const std::filesystem::path process_directory {"/proc/" + std::to_string(pid)};
    struct stat process_stat {};
    if (::stat(process_directory.c_str(), &process_stat) != 0) {
      return std::nullopt;
    }
    std::ifstream stat_file {process_directory / "stat"};
    const std::string stat_contents {std::istreambuf_iterator<char> {stat_file}, {}};
    const auto start_time {process_start_time_from_stat(stat_contents)};
    std::error_code error;
    auto executable {std::filesystem::canonical(process_directory / "exe", error)};
    if (error || executable.empty()) {
      error.clear();
      const auto fallback {gamescope_identity_from_command_line(process_directory)};
      if (!fallback) {
        return std::nullopt;
      }
      executable = *fallback;
    }
    if (!start_time) {
      return std::nullopt;
    }
    return process_identity_t {
      .pid = pid,
      .uid = static_cast<int>(process_stat.st_uid),
      .start_time = *start_time,
      .executable = executable,
    };
#else
    (void) pid;
    return std::nullopt;
#endif
  }

  bool has_gamescope_command_identity(const std::string_view command_line, const std::string_view comm) {
    const auto command_end {command_line.find('\0')};
    const std::filesystem::path command_name {command_line.substr(0, command_end)};
    const bool command_is_gamescope {command_name.filename() == "gamescope" || command_name.filename() == "gamescope-wl"};
    const bool comm_is_gamescope {comm == "gamescope" || comm == "gamescope-wl"};
    return command_is_gamescope && comm_is_gamescope;
  }

  bool has_game_mode_session_identity(const std::string_view command_line, const std::string_view cgroup) {
    size_t component_start {};
    bool steam_session_component {false};
    while (component_start < cgroup.size()) {
      const auto separator {cgroup.find('/', component_start)};
      component_start = separator == std::string_view::npos ? cgroup.size() : separator + 1;
      if (separator == std::string_view::npos) {
        break;
      }
      const auto component_end {cgroup.find_first_of("/\n", component_start)};
      const auto component {cgroup.substr(component_start, component_end - component_start)};
      if (component == "gamescope-session.service" || (component.starts_with("gamescope-session@") && component.ends_with(".service"))) {
        return true;
      }
      if (component == "steam.service" || component == "steam.scope" ||
          ((component.starts_with("steam-") || component.starts_with("app-steam-") || component.starts_with("app-steam@")) &&
           (component.ends_with(".service") || component.ends_with(".scope")))) {
        steam_session_component = true;
      }
      component_start = component_end == std::string_view::npos ? cgroup.size() : component_end;
    }

    const bool steam_argument {command_line.find("--steam") != std::string_view::npos || command_line.find("-steamdeck") != std::string_view::npos};
    return steam_argument && steam_session_component;
  }

  bool is_gamescope_capture_media_class(const std::string_view media_class) {
    return media_class == "Stream/Output/Video" || media_class == "Video/Source";
  }

  bool source_identity_is_current(const gamescope_source_t &source) {
    const auto identity {read_process_identity(source.producer_pid)};
    return identity && identity->uid == source.producer_uid && identity->start_time == source.producer_start_time && identity->executable == source.executable && identity->executable.filename() == "gamescope";
  }

  std::vector<gamescope_source_t> discover_gamescope_sources(const std::string &runtime_directory, const std::string_view remote_name, const std::chrono::milliseconds timeout, std::string &error) {
#if defined(SUNSHINE_BUILD_PIPEWIRE)
    // PipeWire registry discovery owns a separate client connection and can
    // run before any capture backend has initialized the process-wide library.
    // pw_init() is idempotent, so initialize it for this independent path.
    pw_init(nullptr, nullptr);
    auto connection {pipewire_connection_t::connect(runtime_directory, remote_name, error)};
    if (!connection) {
      return {};
    }
    pipewire_registry_t registry;
    if (!registry.connect(std::move(*connection), error)) {
      return {};
    }
    return registry.snapshot(timeout);
#else
    (void) runtime_directory;
    (void) remote_name;
    (void) timeout;
    error = "gamescope_source_pipewire_unavailable";
    return {};
#endif
  }

  std::optional<int> open_host_pipewire_socket(const std::string &runtime_directory, const std::string_view remote_name, std::string &error) {
#if defined(SUNSHINE_BUILD_PIPEWIRE)
    auto connection {pipewire_connection_t::connect(runtime_directory, remote_name, error)};
    return connection ? std::optional<int> {connection->release()} : std::nullopt;
#else
    (void) runtime_directory;
    (void) remote_name;
    error = "gamescope_source_pipewire_unavailable";
    return std::nullopt;
#endif
  }

  std::expected<gamescope_source_t, source_error_e> select_gamescope_source(const std::vector<gamescope_source_t> &sources, const source_selection_request_t &request) {
    using steamos_virtual_session::session_origin_e;
    using steamos_virtual_session::session_source_policy_e;

    if (request.policy == session_source_policy_e::existing_gamescope) {
      return select_unique(sources, request, session_origin_e::attached_existing);
    }
    if (request.policy == session_source_policy_e::owned_private) {
      return select_unique(sources, request, session_origin_e::owned_private);
    }

    const auto existing {select_unique(sources, request, session_origin_e::attached_existing)};
    if (existing.has_value() || existing.error() == source_error_e::ambiguous || request.explicit_gamescope_pid) {
      return existing;
    }
    return select_unique(sources, request, session_origin_e::owned_private);
  }
}  // namespace gamescope_source
