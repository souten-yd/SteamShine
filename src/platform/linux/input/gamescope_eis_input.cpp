/**
 * @file src/platform/linux/input/gamescope_eis_input.cpp
 * @brief Dynamic libei sender implementation for Gamescope input isolation.
 */

#include "gamescope_eis_input.h"

#include "src/logging.h"
#include "src/steamos_virtual_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <dlfcn.h>
#include <mutex>
#include <poll.h>
#include <string_view>
#include <unistd.h>
#include <unordered_set>

using namespace std::chrono_literals;

namespace platf {
  namespace {
    struct ei;
    struct ei_device;
    struct ei_event;
    struct ei_seat;

    /**
     * @brief ABI values shared by supported libei 1.x releases.
     */
    enum class capability_e : int {
      pointer = 1 << 0,
      pointer_absolute = 1 << 1,
      keyboard = 1 << 2,
      scroll = 1 << 4,
      button = 1 << 5,
    };

    /**
     * @brief libei lifecycle event values used by the sender.
     */
    enum class event_type_e : int {
      connect = 1,
      disconnect = 2,
      seat_added = 3,
      seat_removed = 4,
      device_added = 5,
      device_removed = 6,
      device_paused = 7,
      device_resumed = 8,
    };

    /**
     * @brief Resolve a required symbol from a dynamic library.
     *
     * @tparam Function Function-pointer type.
     * @param library Open dynamic-library handle.
     * @param name Exported symbol name.
     * @return Typed symbol, or null when unavailable.
     */
    template<class Function>
    Function symbol(void *library, const char *name) {
      return reinterpret_cast<Function>(::dlsym(library, name));
    }
  }  // namespace

  /**
   * @brief Private dynamic API and connection state.
   */
  class gamescope_eis_input_t::impl_t {
  public:
    /**
     * @brief Function table for the libei 1.x sender ABI.
     */
    struct api_t {
      ei *(*new_sender)(void *);  ///< Create a sender context.
      ei *(*unref)(ei *);  ///< Release a sender context.
      void (*configure_name)(ei *, const char *);  ///< Set the sender application name.
      int (*setup_backend_fd)(ei *, int);  ///< Adopt a verified EIS connection.
      int (*get_fd)(ei *);  ///< Return the event-loop file descriptor.
      void (*dispatch)(ei *);  ///< Dispatch pending protocol input.
      ei_event *(*get_event)(ei *);  ///< Remove the next pending event.
      ei_event *(*event_unref)(ei_event *);  ///< Release an event.
      event_type_e (*event_get_type)(ei_event *);  ///< Return an event type.
      ei_seat *(*event_get_seat)(ei_event *);  ///< Return an event seat.
      ei_device *(*event_get_device)(ei_event *);  ///< Return an event device.
      void (*seat_bind_capabilities)(ei_seat *, ...);  ///< Request seat capabilities.
      bool (*device_has_capability)(ei_device *, capability_e);  ///< Test a device capability.
      ei_device *(*device_ref)(ei_device *);  ///< Retain a device.
      ei_device *(*device_unref)(ei_device *);  ///< Release a device.
      void (*device_start_emulating)(ei_device *, std::uint32_t);  ///< Start an emulation sequence.
      void (*device_stop_emulating)(ei_device *);  ///< Stop an emulation sequence.
      std::uint64_t (*now)(ei *);  ///< Return a compatible monotonic timestamp.
      void (*device_frame)(ei_device *, std::uint64_t);  ///< Commit one device frame.
      void (*pointer_motion)(ei_device *, double, double);  ///< Emit relative pointer motion.
      void (*pointer_motion_absolute)(ei_device *, double, double);  ///< Emit absolute pointer motion.
      void (*button_button)(ei_device *, std::uint32_t, bool);  ///< Emit a button transition.
      void (*scroll_discrete)(ei_device *, std::int32_t, std::int32_t);  ///< Emit discrete scroll.
      void (*keyboard_key)(ei_device *, std::uint32_t, bool);  ///< Emit a keyboard transition.
    };

    /**
     * @brief Release connection and library resources.
     */
    ~impl_t() {
      std::scoped_lock lock {mutex_};
      disconnect_locked();
    }

    /**
     * @brief Prepare delivery for the selected source.
     *
     * @return Current routing result.
     */
    gamescope_input_result_e refresh() {
      std::scoped_lock lock {mutex_};
      return refresh_locked(true);
    }

    /**
     * @brief Send one relative motion event.
     *
     * @param delta_x Horizontal logical-pixel delta.
     * @param delta_y Vertical logical-pixel delta.
     * @return Routing result.
     */
    gamescope_input_result_e move(const int delta_x, const int delta_y) {
      std::scoped_lock lock {mutex_};
      return send_locked(pointer_, [this, delta_x, delta_y](ei_device *device) {
        api_.pointer_motion(device, delta_x, delta_y);
      });
    }

    /**
     * @brief Send one absolute motion event.
     *
     * @param x Horizontal logical-pixel position.
     * @param y Vertical logical-pixel position.
     * @return Routing result.
     */
    gamescope_input_result_e move_absolute(const float x, const float y) {
      std::scoped_lock lock {mutex_};
      return send_locked(pointer_absolute_, [this, x, y](ei_device *device) {
        api_.pointer_motion_absolute(device, x, y);
      });
    }

    /**
     * @brief Send one button transition.
     *
     * @param button Linux input button code.
     * @param pressed True for press and false for release.
     * @return Routing result.
     */
    gamescope_input_result_e button(const std::uint32_t button, const bool pressed) {
      std::scoped_lock lock {mutex_};
      return send_locked(button_, [this, button, pressed](ei_device *device) {
        api_.button_button(device, button, pressed);
      });
    }

    /**
     * @brief Send one scroll event.
     *
     * @param horizontal Horizontal distance in 120-unit wheel clicks.
     * @param vertical Vertical distance in 120-unit wheel clicks.
     * @return Routing result.
     */
    gamescope_input_result_e scroll(const std::int32_t horizontal, const std::int32_t vertical) {
      std::scoped_lock lock {mutex_};
      return send_locked(scroll_, [this, horizontal, vertical](ei_device *device) {
        api_.scroll_discrete(device, horizontal, vertical);
      });
    }

    /**
     * @brief Send one keyboard transition.
     *
     * @param key Linux input key code.
     * @param pressed True for press and false for release.
     * @return Routing result.
     */
    gamescope_input_result_e key(const std::uint32_t key, const bool pressed) {
      std::scoped_lock lock {mutex_};
      return send_locked(keyboard_, [this, key, pressed](ei_device *device) {
        api_.keyboard_key(device, key, pressed);
      });
    }

    /**
     * @brief Return a copy of the current stable error.
     *
     * @return Stable non-secret failure reason.
     */
    std::string error() const {
      std::scoped_lock lock {mutex_};
      return error_;
    }

  private:
    /**
     * @brief Load every symbol required for safe sender operation.
     *
     * @return True when the ABI is complete.
     */
    bool load_api_locked() {
      /// @cond steamshine_internal
#define LOAD_EI(member, name) api_.member = symbol<decltype(api_.member)>(library_, name)
      LOAD_EI(new_sender, "ei_new_sender");
      LOAD_EI(unref, "ei_unref");
      LOAD_EI(configure_name, "ei_configure_name");
      LOAD_EI(setup_backend_fd, "ei_setup_backend_fd");
      LOAD_EI(get_fd, "ei_get_fd");
      LOAD_EI(dispatch, "ei_dispatch");
      LOAD_EI(get_event, "ei_get_event");
      LOAD_EI(event_unref, "ei_event_unref");
      LOAD_EI(event_get_type, "ei_event_get_type");
      LOAD_EI(event_get_seat, "ei_event_get_seat");
      LOAD_EI(event_get_device, "ei_event_get_device");
      LOAD_EI(seat_bind_capabilities, "ei_seat_bind_capabilities");
      LOAD_EI(device_has_capability, "ei_device_has_capability");
      LOAD_EI(device_ref, "ei_device_ref");
      LOAD_EI(device_unref, "ei_device_unref");
      LOAD_EI(device_start_emulating, "ei_device_start_emulating");
      LOAD_EI(device_stop_emulating, "ei_device_stop_emulating");
      LOAD_EI(now, "ei_now");
      LOAD_EI(device_frame, "ei_device_frame");
      LOAD_EI(pointer_motion, "ei_device_pointer_motion");
      LOAD_EI(pointer_motion_absolute, "ei_device_pointer_motion_absolute");
      LOAD_EI(button_button, "ei_device_button_button");
      LOAD_EI(scroll_discrete, "ei_device_scroll_discrete");
      LOAD_EI(keyboard_key, "ei_device_keyboard_key");
#undef LOAD_EI
      /// @endcond
      return api_.new_sender &&
             api_.unref &&
             api_.configure_name &&
             api_.setup_backend_fd &&
             api_.get_fd &&
             api_.dispatch &&
             api_.get_event &&
             api_.event_unref &&
             api_.event_get_type &&
             api_.event_get_seat &&
             api_.event_get_device &&
             api_.seat_bind_capabilities &&
             api_.device_has_capability &&
             api_.device_ref &&
             api_.device_unref &&
             api_.device_start_emulating &&
             api_.device_stop_emulating &&
             api_.now &&
             api_.device_frame &&
             api_.pointer_motion &&
             api_.pointer_motion_absolute &&
             api_.button_button &&
             api_.scroll_discrete &&
             api_.keyboard_key;
    }

    /**
     * @brief Connect to the verified endpoint and await advertised devices.
     */
    gamescope_input_result_e refresh_locked(const bool wait_for_capabilities) {
      if (!steamos_virtual_session::gamescope_input_required()) {
        disconnect_locked();
        retry_after_ = {};
        return gamescope_input_result_e::desktop;
      }
      if (!context_ && std::chrono::steady_clock::now() < retry_after_) {
        return gamescope_input_result_e::blocked;
      }

      if (context_) {
        std::uint64_t active_generation {};
        if (!steamos_virtual_session::gamescope_input_generation(active_generation, error_) || active_generation != generation_) {
          disconnect_locked(false);
        }
      }
      if (context_) {
        if (!pump_locked()) {
          disconnect_locked(false);
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
        if (ready_locked()) {
          error_.clear();
          return gamescope_input_result_e::delivered;
        }
        if (!wait_for_capabilities) {
          error_ = "gamescope_input_eis_negotiating";
          return gamescope_input_result_e::blocked;
        }
      }
      if (!context_) {
        disconnect_locked();
        std::string socket_path;
        int input_descriptor {-1};
        std::uint64_t generation {};
        if (!steamos_virtual_session::open_verified_gamescope_input(socket_path, input_descriptor, generation, error_)) {
          disconnect_locked(false);
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
        socket_path_ = std::move(socket_path);
        generation_ = generation;
        library_ = ::dlopen("libei.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library_) {
          ::close(input_descriptor);
          error_ = "gamescope_input_libei_unavailable";
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
        if (!load_api_locked()) {
          ::close(input_descriptor);
          error_ = "gamescope_input_libei_abi_incomplete";
          disconnect_locked(false);
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
        context_ = api_.new_sender(nullptr);
        if (!context_) {
          ::close(input_descriptor);
          error_ = "gamescope_input_libei_context_failed";
          disconnect_locked(false);
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
        api_.configure_name(context_, "SteamShine");
        if (api_.setup_backend_fd(context_, input_descriptor) < 0) {
          error_ = "gamescope_input_eis_connect_failed";
          disconnect_locked(false);
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
      }

      if (!wait_for_capabilities) {
        if (!pump_locked()) {
          disconnect_locked(false);
          retry_after_ = std::chrono::steady_clock::now() + 1s;
          return gamescope_input_result_e::blocked;
        }
        error_ = "gamescope_input_eis_negotiating";
        return gamescope_input_result_e::blocked;
      }

      const auto deadline {std::chrono::steady_clock::now() + 500ms};
      while (std::chrono::steady_clock::now() < deadline && !ready_locked()) {
        pollfd descriptor {
          .fd = api_.get_fd(context_),
          .events = POLLIN,
          .revents = 0,
        };
        const auto remaining {std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())};
        if (::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count()))) < 0 || !pump_locked()) {
          break;
        }
      }
      if (!ready_locked()) {
        error_ = "gamescope_input_eis_capabilities_unavailable";
        disconnect_locked(false);
        retry_after_ = std::chrono::steady_clock::now() + 1s;
        return gamescope_input_result_e::blocked;
      }
      error_.clear();
      retry_after_ = {};
      BOOST_LOG(info) << "Gamescope input is isolated through verified EIS endpoint: " << socket_path_;
      return gamescope_input_result_e::delivered;
    }

    /**
     * @brief Process pending lifecycle events from libei.
     *
     * @return False after disconnect.
     */
    bool pump_locked() {
      if (!context_) {
        return false;
      }
      api_.dispatch(context_);
      while (auto *event = api_.get_event(context_)) {
        const auto type {api_.event_get_type(event)};
        auto *device {api_.event_get_device(event)};
        if (type == event_type_e::seat_added) {
          api_.seat_bind_capabilities(
            api_.event_get_seat(event),
            capability_e::pointer,
            capability_e::pointer_absolute,
            capability_e::keyboard,
            capability_e::button,
            capability_e::scroll,
            nullptr
          );
        } else if (type == event_type_e::device_added) {
          assign_device_locked(device);
        } else if (type == event_type_e::device_resumed) {
          resumed_.insert(device);
          api_.device_start_emulating(device, ++sequence_);
        } else if (type == event_type_e::device_paused) {
          resumed_.erase(device);
        } else if (type == event_type_e::device_removed) {
          remove_device_locked(device);
        } else if (type == event_type_e::disconnect) {
          api_.event_unref(event);
          error_ = "gamescope_input_eis_disconnected";
          return false;
        }
        api_.event_unref(event);
      }
      return true;
    }

    /**
     * @brief Retain a device for each capability it advertises.
     */
    void assign_device_locked(ei_device *device) {
      assign_capability_locked(pointer_, device, capability_e::pointer);
      assign_capability_locked(pointer_absolute_, device, capability_e::pointer_absolute);
      assign_capability_locked(keyboard_, device, capability_e::keyboard);
      assign_capability_locked(button_, device, capability_e::button);
      assign_capability_locked(scroll_, device, capability_e::scroll);
    }

    /**
     * @brief Retain one advertised capability device if no prior device exists.
     */
    void assign_capability_locked(ei_device *&destination, ei_device *device, const capability_e capability) {
      if (!destination && api_.device_has_capability(device, capability)) {
        destination = api_.device_ref(device);
      }
    }

    /**
     * @brief Remove all retained references matching a removed device.
     */
    void remove_device_locked(ei_device *device) {
      resumed_.erase(device);
      for (auto **slot : {&pointer_, &pointer_absolute_, &keyboard_, &button_, &scroll_}) {
        if (*slot == device) {
          *slot = api_.device_unref(*slot);
        }
      }
    }

    /**
     * @brief Test whether all pointer and keyboard paths are active.
     */
    bool ready_locked() const {
      return active_locked(pointer_) &&
             active_locked(pointer_absolute_) &&
             active_locked(keyboard_) &&
             active_locked(button_) &&
             active_locked(scroll_);
    }

    /**
     * @brief Test whether a retained device is resumed.
     */
    bool active_locked(ei_device *device) const {
      return device && resumed_.contains(device);
    }

    /**
     * @brief Route and frame one event.
     */
    template<class Sender>
    gamescope_input_result_e send_locked(ei_device *&device, Sender sender) {
      const auto route {refresh_locked(false)};
      if (route != gamescope_input_result_e::delivered) {
        return route;
      }
      if (!active_locked(device)) {
        error_ = "gamescope_input_eis_device_paused";
        return gamescope_input_result_e::blocked;
      }
      sender(device);
      api_.device_frame(device, api_.now(context_));
      return gamescope_input_result_e::delivered;
    }

    /**
     * @brief Stop emulation and release dynamic resources.
     *
     * @param clear_error Whether to clear the diagnostic reason.
     */
    void disconnect_locked(const bool clear_error = true) {
      if (context_) {
        std::unordered_set<ei_device *> devices;
        for (auto *device : {pointer_, pointer_absolute_, keyboard_, button_, scroll_}) {
          if (device) {
            devices.insert(device);
          }
        }
        for (auto *device : devices) {
          if (resumed_.contains(device)) {
            api_.device_stop_emulating(device);
          }
        }
        for (auto **slot : {&pointer_, &pointer_absolute_, &keyboard_, &button_, &scroll_}) {
          if (*slot) {
            *slot = api_.device_unref(*slot);
          }
        }
        context_ = api_.unref(context_);
      }
      resumed_.clear();
      api_ = {};
      if (library_) {
        ::dlclose(library_);
        library_ = nullptr;
      }
      socket_path_.clear();
      generation_ = 0;
      if (clear_error) {
        error_.clear();
      }
    }

    mutable std::mutex mutex_;  ///< Serializes libei dispatch and emission.
    void *library_ {};  ///< Dynamic libei handle.
    api_t api_ {};  ///< Resolved libei ABI.
    ei *context_ {};  ///< Connected sender context.
    ei_device *pointer_ {};  ///< Relative pointer device.
    ei_device *pointer_absolute_ {};  ///< Absolute pointer device.
    ei_device *keyboard_ {};  ///< Keyboard device.
    ei_device *button_ {};  ///< Pointer-button device.
    ei_device *scroll_ {};  ///< Scroll device.
    std::unordered_set<ei_device *> resumed_;  ///< Devices currently allowed to emit.
    std::uint32_t sequence_ {};  ///< Monotonic emulation transaction sequence.
    std::string socket_path_;  ///< Verified endpoint used by this connection.
    std::uint64_t generation_ {};  ///< Session identity bound to this connection.
    std::string error_;  ///< Stable failure reason.
    std::chrono::steady_clock::time_point retry_after_ {};  ///< Earliest reconnect retry.
  };

  gamescope_eis_input_t::gamescope_eis_input_t():
      impl_ {std::make_unique<impl_t>()} {
  }

  gamescope_eis_input_t::~gamescope_eis_input_t() = default;

  gamescope_input_result_e gamescope_eis_input_t::refresh() {
    return impl_->refresh();
  }

  gamescope_input_result_e gamescope_eis_input_t::move(const int delta_x, const int delta_y) {
    return impl_->move(delta_x, delta_y);
  }

  gamescope_input_result_e gamescope_eis_input_t::move_absolute(const float x, const float y) {
    return impl_->move_absolute(x, y);
  }

  gamescope_input_result_e gamescope_eis_input_t::button(const std::uint32_t button, const bool pressed) {
    return impl_->button(button, pressed);
  }

  gamescope_input_result_e gamescope_eis_input_t::scroll(const std::int32_t horizontal, const std::int32_t vertical) {
    return impl_->scroll(horizontal, vertical);
  }

  gamescope_input_result_e gamescope_eis_input_t::key(const std::uint32_t key, const bool pressed) {
    return impl_->key(key, pressed);
  }

  std::string gamescope_eis_input_t::error() const {
    return impl_->error();
  }
}  // namespace platf
