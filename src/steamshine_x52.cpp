/**
 * @file src/steamshine_x52.cpp
 * @brief X52 profile storage and the standard X52 vendor display protocol.
 */
#include "steamshine_x52.h"

#include "config.h"
#include "steamshine_gamepad_shortcuts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>

#if defined(__linux__)
  #include <fcntl.h>
  #include <linux/input.h>
  #include <linux/uinput.h>
  #include <sys/ioctl.h>
  #include <unistd.h>
#endif

namespace steamshine_x52 {
  namespace {
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    constexpr std::string_view CONFIG_KEY {"steamshine_x52_profiles"};  ///< Persistent configuration entry.
    constexpr std::string_view DEFAULT_NAME {"Recommended"};  ///< Built-in profile name.
    constexpr std::size_t MAX_PROFILES {12};  ///< Maximum saved profiles, including the default.
    std::mutex state_mutex;  ///< Protects the in-memory profile store.
    std::optional<json> state;  ///< Loaded profile store.
    std::uint64_t generation {0};  ///< Incremented on each profile change.
    std::atomic<int> last_button {-1};  ///< Last X52 button pressed for control identification.

    /**
     * @brief Return an X52 USB device's bus node when a supported device is present.
     *
     * @return USB node and stable sysfs identity, or no value when disconnected.
     */
    std::optional<std::pair<fs::path, std::string>> connected_device() {
#if defined(__linux__)
      std::error_code error;
      for (const auto &entry : fs::directory_iterator("/sys/bus/usb/devices", error)) {
        if (error) {
          break;
        }
        std::ifstream vendor {entry.path() / "idVendor"};
        std::ifstream product {entry.path() / "idProduct"};
        std::string vendor_id, product_id;
        vendor >> vendor_id;
        product >> product_id;
        if (vendor_id != "06a3" || (product_id != "075c" && product_id != "0255")) {
          continue;
        }
        std::ifstream bus_file {entry.path() / "busnum"};
        std::ifstream device_file {entry.path() / "devnum"};
        int bus {0}, device {0};
        if (!(bus_file >> bus) || !(device_file >> device) || bus < 1 || device < 1) {
          continue;
        }
        return std::pair {fs::path {std::format("/dev/bus/usb/{:03}/{:03}", bus, device)}, entry.path().filename().string()};
      }
#endif
      return std::nullopt;
    }

    /**
     * @brief Read a bounded integer setting from a JSON object.
     *
     * @param source Input JSON object.
     * @param key Setting name.
     * @param minimum Lowest accepted value.
     * @param maximum Highest accepted value.
     * @return Parsed value, or no value when invalid.
     */
    std::optional<int> bounded_int(const json &source, const char *key, const int minimum, const int maximum) {
      if (!source.contains(key) || !source[key].is_number_integer()) {
        return std::nullopt;
      }
      if (source[key].is_number_unsigned()) {
        const auto unsigned_value {source[key].get<std::uint64_t>()};
        return unsigned_value >= static_cast<std::uint64_t>(std::max(minimum, 0)) && unsigned_value <= static_cast<std::uint64_t>(maximum) ? std::optional<int> {static_cast<int>(unsigned_value)} : std::nullopt;
      }
      const auto value {source[key].get<std::int64_t>()};
      if (value < minimum || value > maximum) {
        return std::nullopt;
      }
      return static_cast<int>(value);
    }

    /**
     * @brief Build a safe profile store from the saved configuration text.
     *
     * @return Default store or a validated saved store.
     */
    json load_state() {
      json result {{"active", std::string {DEFAULT_NAME}}, {"input_enabled", true}, {"profiles", json::array({recommended_profile()})}};
      const auto parsed = json::parse(config::sunshine.steamshine_x52_profiles, nullptr, false);
      if (!parsed.is_object() || !parsed.contains("profiles") || !parsed["profiles"].is_array() || !parsed.contains("active") || !parsed["active"].is_string()) {
        return result;
      }
      if (parsed["profiles"].size() > MAX_PROFILES) {
        return result;
      }
      if (parsed.contains("input_enabled") && parsed["input_enabled"].is_boolean()) {
        result["input_enabled"] = parsed["input_enabled"];
      }
      for (const auto &candidate : parsed["profiles"]) {
        std::string error;
        auto profile = validate_profile(candidate, error);
        if (profile.is_discarded()) {
          continue;
        }
        if (profile["name"] == DEFAULT_NAME) {
          result["profiles"][0]["enabled"] = profile["enabled"];
          continue;
        }
        if (result["profiles"].size() < MAX_PROFILES) {
          result["profiles"].push_back(std::move(profile));
        }
      }
      for (const auto &profile : result["profiles"]) {
        if (profile["name"] == parsed["active"]) {
          result["active"] = parsed["active"];
        }
      }
      return result;
    }

    /**
     * @brief Return the in-memory store under the caller's lock.
     *
     * @return Mutable profile store.
     */
    json &current_state() {
      if (!state) {
        state = load_state();
      }
      return *state;
    }

    /**
     * @brief Send the vendor control request defined for the X52 display.
     *
     * @param descriptor Open USB device descriptor.
     * @param index Vendor command index.
     * @param value Vendor command value.
     * @return True when the device accepted the request.
     */
    bool command(const int descriptor, const std::uint16_t index, const std::uint16_t value) {
#if defined(__linux__)
      struct control_transfer_t {
        std::uint8_t request_type;  ///< USB vendor, device, outbound.
        std::uint8_t request;  ///< X52 command request ID.
        std::uint16_t value;  ///< Command payload.
        std::uint16_t index;  ///< Command selector.
        std::uint16_t length;  ///< No data stage.
        std::uint32_t timeout;  ///< USB timeout in milliseconds.
        void *data;  ///< No data stage buffer.
      } transfer {0x40, 0x91, value, index, 0, 1000, nullptr};

      return ::ioctl(descriptor, _IOWR('U', 0, control_transfer_t), &transfer) == 0;
#else
      (void) descriptor;
      (void) index;
      (void) value;
      return false;
#endif
    }

    /**
     * @brief Apply one profile's controls to an open X52.
     *
     * @param descriptor Open USB device descriptor.
     * @param profile Validated profile.
     * @return True when every transfer succeeded.
     */
    bool apply_to_device(const int descriptor, const json &profile) {
      const auto &display {profile["display"]};
      const auto &lighting {profile["lighting"]};
      if (!command(descriptor, 0xb1, display["brightness"].get<int>()) || !command(descriptor, 0xb2, lighting["brightness"].get<int>()) ||
          !command(descriptor, 0xb4, lighting["blink"].get<bool>() ? 0x51 : 0x50) ||
          !command(descriptor, 0xfd, display["shift"].get<bool>() ? 0x51 : 0x50)) {
        return false;
      }
      const std::array<std::uint16_t, 3> line_ids {0xd1, 0xd2, 0xd4};
      for (std::size_t line = 0; line < line_ids.size(); ++line) {
        if (!command(descriptor, line_ids[line] | 0x08, 0)) {
          return false;
        }
        auto text {display["lines"][line].get<std::string>()};
        if (text.size() % 2) {
          text.push_back(' ');
        }
        for (std::size_t position = 0; position < text.size(); position += 2) {
          const auto value {static_cast<std::uint16_t>(static_cast<unsigned char>(text[position]) | (static_cast<unsigned char>(text[position + 1]) << 8))};
          if (!command(descriptor, line_ids[line], value)) {
            return false;
          }
        }
      }
      const auto now {std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};
      std::tm local {};
#if defined(__linux__)
      ::localtime_r(&now, &local);
#else
      local = *std::localtime(&now);
#endif
      const int year {(local.tm_year + 1900) % 100};
      const int month {local.tm_mon + 1};
      const int day {local.tm_mday};
      const auto date_format {display["date_format"].get<std::string>()};
      const std::uint16_t date_first {static_cast<std::uint16_t>(date_format == "YYMMDD" ? (month << 8) | year : date_format == "MMDDYY" ? (day << 8) | month :
                                                                                                                                           (month << 8) | day)};
      const std::uint16_t date_last {static_cast<std::uint16_t>(date_format == "YYMMDD" ? day : year)};
      const bool hours24 {display["hours_24"].get<bool>()};
      if (!command(descriptor, 0xc4, date_first) || !command(descriptor, 0xc8, date_last) ||
          !command(descriptor, 0xc0, static_cast<std::uint16_t>((hours24 ? 0x8000 : 0) | (local.tm_hour << 8) | local.tm_min))) {
        return false;
      }
      for (int clock = 0; clock < 2; ++clock) {
        const auto offset {display["clock_offsets_minutes"][clock].get<int>()};
        const auto magnitude {std::abs(offset)};
        const auto value {static_cast<std::uint16_t>((hours24 ? 0x8000 : 0) | (offset < 0 ? 0x400 : 0) | magnitude)};
        if (!command(descriptor, static_cast<std::uint16_t>(0xc1 + clock), value)) {
          return false;
        }
      }
      return true;
    }

    /**
     * @brief Apply a profile to the connected X52, opening only the detected USB node.
     *
     * @param device Verified X52 USB device.
     * @param profile Validated active profile.
     * @return True when no transfer failed.
     */
    bool apply(const std::pair<fs::path, std::string> &device, const json &profile) {
#if defined(__linux__)
      const int descriptor {::open(device.first.c_str(), O_RDWR | O_CLOEXEC)};
      if (descriptor < 0) {
        return false;
      }
      const bool success {apply_to_device(descriptor, profile)};
      ::close(descriptor);
      return success;
#else
      (void) device;
      (void) profile;
      return false;
#endif
    }

    /**
     * @brief Persist the profile store in Sunshine's configuration file.
     *
     * @param candidate Store to write.
     * @param error Failure reason.
     * @return True when the file was replaced.
     */
    bool persist(const json &candidate, std::string &error) {
      return steamshine_gamepad_shortcuts::write_config_value(CONFIG_KEY, candidate.dump(), {}, error);
    }

#if defined(__linux__)
    /**
     * @brief Translate an allowed profile key into a Linux keyboard code.
     *
     * @param key Uppercase key name from a validated profile.
     * @return Linux key code, or zero for no assignment.
     */
    int key_code(const std::string &key) {
      if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
        constexpr std::array<int, 26> letters {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        return letters[key[0] - 'A'];
      }
      if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
        constexpr std::array<int, 10> digits {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
        return digits[key[0] - '0'];
      }
      constexpr std::array<std::pair<std::string_view, int>, 23> named {{
        {"SPACE", KEY_SPACE},
        {"ENTER", KEY_ENTER},
        {"ESC", KEY_ESC},
        {"TAB", KEY_TAB},
        {"UP", KEY_UP},
        {"DOWN", KEY_DOWN},
        {"LEFT", KEY_LEFT},
        {"RIGHT", KEY_RIGHT},
        {"F1", KEY_F1},
        {"F2", KEY_F2},
        {"F3", KEY_F3},
        {"F4", KEY_F4},
        {"F5", KEY_F5},
        {"F6", KEY_F6},
        {"F7", KEY_F7},
        {"F8", KEY_F8},
        {"F9", KEY_F9},
        {"F10", KEY_F10},
        {"F11", KEY_F11},
        {"F12", KEY_F12},
        {"CTRL", KEY_LEFTCTRL},
        {"ALT", KEY_LEFTALT},
        {"SHIFT", KEY_LEFTSHIFT},
      }};
      const auto found {std::ranges::find(named, key, &std::pair<std::string_view, int>::first)};
      return found == named.end() ? 0 : found->second;
    }

    /**
     * @brief Find the event device belonging to a standard X52.
     *
     * @return Input event node, or no value when unavailable.
     */
    std::optional<fs::path> event_device() {
      std::error_code error;
      for (const auto &entry : fs::directory_iterator("/sys/class/input", error)) {
        if (error || !entry.path().filename().string().starts_with("event")) {
          continue;
        }
        std::ifstream vendor {entry.path() / "device/id/vendor"};
        std::ifstream product {entry.path() / "device/id/product"};
        std::string vendor_id, product_id;
        vendor >> vendor_id;
        product >> product_id;
        if (vendor_id == "06a3" && (product_id == "075c" || product_id == "0255")) {
          return fs::path {"/dev/input"} / entry.path().filename();
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Send a keyboard event and its sync marker through uinput.
     *
     * @param descriptor Open virtual keyboard descriptor.
     * @param code Linux key code.
     * @param value Press or release value.
     */
    void emit_key(const int descriptor, const int code, const int value) {
      input_event key {};
      key.type = EV_KEY;
      key.code = code;
      key.value = value;
      input_event sync {};
      sync.type = EV_SYN;
      sync.code = SYN_REPORT;
      ::write(descriptor, &key, sizeof(key));
      ::write(descriptor, &sync, sizeof(sync));
    }

    /**
     * @brief Open a read-only X52 input stream and a virtual keyboard for assignments.
     *
     * @param profile Active validated profile.
     * @param enabled Whether keyboard assignments should be emitted.
     * @param input_enabled Whether physical events should reach other applications.
     * @return Input and uinput descriptors, or closed descriptors on failure.
     */
    std::pair<int, int> open_mapped_input(const json &profile, const bool enabled, const bool input_enabled) {
      const auto event {event_device()};
      if (!event) {
        return {-1, -1};
      }
      const int input {::open(event->c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
      if (input < 0) {
        return {-1, -1};
      }
      if (!input_enabled && ::ioctl(input, EVIOCGRAB, 1) != 0) {
        ::close(input);
        return {-1, -1};
      }
      if (!input_enabled) {
        return {input, -1};
      }
      if (!enabled || profile["buttons"].empty()) {
        return {input, -1};
      }
      const int output {::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)};
      if (output < 0) {
        return {input, -1};
      }
      bool success {::ioctl(output, UI_SET_EVBIT, EV_KEY) == 0};
      for (const auto &key : profile["buttons"].items()) {
        success = ::ioctl(output, UI_SET_KEYBIT, key_code(key.value().get<std::string>())) == 0 && success;
      }
      uinput_setup setup {};
      std::snprintf(setup.name, sizeof(setup.name), "SteamShine X52 key assignments");
      setup.id.bustype = BUS_VIRTUAL;
      setup.id.vendor = 0x1d6b;
      setup.id.product = 0x5552;
      success = ::ioctl(output, UI_DEV_SETUP, &setup) == 0 && success;
      success = ::ioctl(output, UI_DEV_CREATE) == 0 && success;
      if (!success) {
        ::close(output);
        return {input, -1};
      }
      return {input, output};
    }

    /**
     * @brief Destroy a virtual keyboard and close its input source.
     *
     * @param input Open input descriptor, if any.
     * @param output Open virtual keyboard descriptor, if any.
     */
    void close_mapped_input(const int input, const int output) {
      if (input >= 0) {
        ::close(input);
      }
      if (output >= 0) {
        ::ioctl(output, UI_DEV_DESTROY);
        ::close(output);
      }
    }

    /**
     * @brief Forward currently pending configured X52 button presses as keys.
     *
     * @param input Open physical X52 event descriptor.
     * @param output Open virtual keyboard descriptor, or -1 for identification only.
     * @param profile Active profile.
     */
    void process_buttons(const int input, const int output, const json &profile) {
      input_event event {};
      while (::read(input, &event, sizeof(event)) == sizeof(event)) {
        if (event.type != EV_KEY || (event.value != 0 && event.value != 1)) {
          continue;
        }
        const int button {event.code >= BTN_JOYSTICK && event.code < BTN_JOYSTICK + 16 ? event.code - BTN_JOYSTICK : event.code >= BTN_TRIGGER_HAPPY1 && event.code < BTN_TRIGGER_HAPPY1 + 18 ? event.code - BTN_TRIGGER_HAPPY1 + 16 :
                                                                                                                                                                                                -1};
        if (button >= 0 && event.value == 1) {
          last_button.store(button);
        }
        if (button < 0 || output < 0 || !profile["buttons"].contains(std::to_string(button))) {
          continue;
        }
        emit_key(output, key_code(profile["buttons"][std::to_string(button)].get<std::string>()), event.value);
      }
    }
#endif
  }  // namespace

  nlohmann::json recommended_profile() {
    return {{"name", std::string {DEFAULT_NAME}}, {"enabled", false}, {"buttons", json::object()}, {"lighting", {{"brightness", 64}, {"blink", false}}}, {"display", {{"brightness", 80}, {"shift", false}, {"lines", json::array({"SteamShine X52", "Ready", ""})}, {"hours_24", true}, {"date_format", "YYMMDD"}, {"clock_offsets_minutes", json::array({0, 0})}}}};
  }

  nlohmann::json validate_profile(const nlohmann::json &input, std::string &error) {
    if (!input.is_object() || !input.contains("name") || !input["name"].is_string() || !input.contains("enabled") || !input["enabled"].is_boolean() ||
        !input.contains("lighting") || !input["lighting"].is_object() || !input.contains("display") || !input["display"].is_object()) {
      error = "Profile name, enabled state, lighting, and display are required";
      return json::value_t::discarded;
    }
    const auto name {input["name"].get<std::string>()};
    if (name.empty() || name.size() > 40 || name.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
      error = "Profile name must be 1 to 40 characters on one line";
      return json::value_t::discarded;
    }
    const auto &lighting {input["lighting"]};
    const auto &display {input["display"]};
    const auto led_brightness {bounded_int(lighting, "brightness", 0, 128)};
    const auto mfd_brightness {bounded_int(display, "brightness", 0, 128)};
    if (!led_brightness || !mfd_brightness || !lighting.contains("blink") || !lighting["blink"].is_boolean() ||
        !display.contains("shift") || !display["shift"].is_boolean() || !display.contains("hours_24") || !display["hours_24"].is_boolean() ||
        !display.contains("date_format") || !display["date_format"].is_string() ||
        !display.contains("lines") || !display["lines"].is_array() || display["lines"].size() != 3 ||
        !display.contains("clock_offsets_minutes") || !display["clock_offsets_minutes"].is_array() || display["clock_offsets_minutes"].size() != 2) {
      error = "X52 lighting or display settings are invalid";
      return json::value_t::discarded;
    }
    const auto date_format {display["date_format"].get<std::string>()};
    if (date_format != "DDMMYY" && date_format != "MMDDYY" && date_format != "YYMMDD") {
      error = "Choose a supported date format";
      return json::value_t::discarded;
    }
    for (const auto &line : display["lines"]) {
      if (!line.is_string()) {
        error = "Each MFD line must be text";
        return json::value_t::discarded;
      }
      const auto value {line.get<std::string>()};
      if (value.size() > 16 || std::ranges::any_of(value, [](const unsigned char character) {
            return character < 0x20 || character > 0x7e;
          })) {
        error = "MFD lines must contain at most 16 printable ASCII characters";
        return json::value_t::discarded;
      }
    }
    for (const auto &offset : display["clock_offsets_minutes"]) {
      if (!offset.is_number_integer() || (offset.is_number_unsigned() && offset.get<std::uint64_t>() > 1023) ||
          (!offset.is_number_unsigned() && (offset.get<std::int64_t>() < -1023 || offset.get<std::int64_t>() > 1023))) {
        error = "Clock offsets must be between -1023 and 1023 minutes";
        return json::value_t::discarded;
      }
    }
    json buttons = json::object();
    if (input.contains("buttons")) {
      if (!input["buttons"].is_object() || input["buttons"].size() > 34) {
        error = "Button assignments must be an object of X52 button numbers";
        return json::value_t::discarded;
      }
      constexpr std::array<std::string_view, 24> KEYS {"NONE", "SPACE", "ENTER", "ESC", "TAB", "UP", "DOWN", "LEFT", "RIGHT", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "CTRL", "ALT", "SHIFT"};
      for (auto entry = input["buttons"].begin(); entry != input["buttons"].end(); ++entry) {
        int button = -1;
        try {
          const auto parsed {std::stoi(entry.key())};
          if (std::to_string(parsed) == entry.key()) {
            button = parsed;
          }
        } catch (...) {
        }
        if (button < 0 || button >= 34 || !entry.value().is_string()) {
          error = "Choose a valid X52 button and keyboard key";
          return json::value_t::discarded;
        }
        const auto key {entry.value().get<std::string>()};
        if (std::ranges::find(KEYS, key) == KEYS.end() && !(key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') &&
            !(key.size() == 1 && key[0] >= '0' && key[0] <= '9')) {
          error = "Choose a supported keyboard key";
          return json::value_t::discarded;
        }
        if (key != "NONE") {
          buttons[entry.key()] = key;
        }
      }
    }
    return {{"name", name}, {"enabled", input["enabled"]}, {"buttons", std::move(buttons)}, {"lighting", {{"brightness", *led_brightness}, {"blink", lighting["blink"]}}}, {"display", {{"brightness", *mfd_brightness}, {"shift", display["shift"]}, {"lines", display["lines"]}, {"hours_24", display["hours_24"]}, {"date_format", date_format}, {"clock_offsets_minutes", display["clock_offsets_minutes"]}}}};
  }

  nlohmann::json status() {
    std::lock_guard lock {state_mutex};
    auto result = current_state();
    const auto device {connected_device()};
    result["connected"] = device.has_value();
    result["device"] = device ? "Logitech / Saitek X52 H.O.T.A.S." : "";
    result["capabilities"] = {{"individual_led_colors", false}, {"mfd_lines", 3}, {"mfd_characters", 16}, {"brightness_max", 128}};
    result["last_button"] = last_button.load();
#if defined(__linux__)
    const auto event {event_device()};
    result["keyboard_ready"] = device && event && ::access(event->c_str(), R_OK) == 0 && ::access("/dev/uinput", W_OK) == 0;
#else
    result["keyboard_ready"] = false;
#endif
    return result;
  }

  bool save(const nlohmann::json &input, std::string &error) {
    std::lock_guard lock {state_mutex};
    const auto device {connected_device()};
    if (!device) {
      error = "Connect an X52 HOTAS before changing its addon settings";
      return false;
    }
    if (!input.is_object() || !input.contains("action") || !input["action"].is_string()) {
      error = "Choose a profile action";
      return false;
    }
    auto candidate = current_state();
    const auto action {input["action"].get<std::string>()};
    if (action == "save") {
      if (!input.contains("profile")) {
        error = "A profile is required";
        return false;
      }
      auto profile = validate_profile(input["profile"], error);
      if (profile.is_discarded()) {
        return false;
      }
      const auto name {profile["name"].get<std::string>()};
      if (name == DEFAULT_NAME) {
        error = "The recommended profile is read-only; save a copy under another name";
        return false;
      }
      auto &profiles {candidate["profiles"]};
      const auto existing {std::ranges::find_if(profiles, [&](const auto &entry) {
        return entry["name"] == name;
      })};
      if (existing == profiles.end()) {
        if (profiles.size() >= MAX_PROFILES) {
          error = "The X52 profile limit has been reached";
          return false;
        }
        profiles.push_back(std::move(profile));
      } else {
        *existing = std::move(profile);
      }
      candidate["active"] = name;
    } else if (action == "activate" || action == "delete") {
      if (!input.contains("name") || !input["name"].is_string()) {
        error = "Choose a saved profile";
        return false;
      }
      const auto name {input["name"].get<std::string>()};
      auto &profiles {candidate["profiles"]};
      const auto existing {std::ranges::find_if(profiles, [&](const auto &entry) {
        return entry["name"] == name;
      })};
      if (existing == profiles.end() || (action == "delete" && name == DEFAULT_NAME)) {
        error = "That profile cannot be changed";
        return false;
      }
      if (action == "activate") {
        candidate["active"] = name;
      } else {
        profiles.erase(existing);
        if (candidate["active"] == name) {
          candidate["active"] = DEFAULT_NAME;
        }
      }
    } else if (action == "enable") {
      if (!input.contains("enabled") || !input["enabled"].is_boolean()) {
        error = "An enabled state is required";
        return false;
      }
      for (auto &profile : candidate["profiles"]) {
        if (profile["name"] == candidate["active"]) {
          profile["enabled"] = input["enabled"];
        }
      }
    } else if (action == "input") {
      if (!input.contains("enabled") || !input["enabled"].is_boolean()) {
        error = "An input enabled state is required";
        return false;
      }
#if defined(__linux__)
      if (!input["enabled"].get<bool>() && candidate.value("input_enabled", true)) {
        const auto event {event_device()};
        const int descriptor {event ? ::open(event->c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC) : -1};
        if (descriptor < 0 || ::ioctl(descriptor, EVIOCGRAB, 1) != 0) {
          if (descriptor >= 0) {
            ::close(descriptor);
          }
          error = "The X52 input device could not be blocked";
          return false;
        }
        ::close(descriptor);
      }
#endif
      candidate["input_enabled"] = input["enabled"];
    } else {
      error = "Unsupported profile action";
      return false;
    }
    const auto active {std::ranges::find_if(candidate["profiles"], [&](const auto &entry) {
      return entry["name"] == candidate["active"];
    })};
    if (action != "input" && active != candidate["profiles"].end() && (*active)["enabled"].get<bool>() && !apply(*device, *active)) {
      error = "The X52 rejected its display settings or USB access was denied";
      return false;
    }
    if (!persist(candidate, error)) {
      return false;
    }
    config::sunshine.steamshine_x52_profiles = candidate.dump();
    state = std::move(candidate);
    ++generation;
    return true;
  }

  void monitor(const std::stop_token stop) {
    std::string previous_device;
    std::uint64_t previous_generation {0};
#if defined(__linux__)
    int input {-1}, output {-1};
#endif
    while (!stop.stop_requested()) {
      const auto device {connected_device()};
      json active;
      bool input_enabled {true};
      std::uint64_t current_generation {0};
      {
        std::lock_guard lock {state_mutex};
        const auto &store {current_state()};
        const auto found {std::ranges::find_if(store["profiles"], [&](const auto &entry) {
          return entry["name"] == store["active"];
        })};
        if (found != store["profiles"].end()) {
          active = *found;
        }
        current_generation = generation;
        input_enabled = store.value("input_enabled", true);
      }
      const auto identity {device ? device->first.string() : std::string {}};
      if (identity != previous_device || current_generation != previous_generation) {
#if defined(__linux__)
        close_mapped_input(input, output);
        input = output = -1;
        if (identity != previous_device) {
          last_button.store(-1);
        }
#endif
        if (device && !active.is_null() && active.value("enabled", false)) {
          apply(*device, active);
        }
#if defined(__linux__)
        if (device && !active.is_null()) {
          std::tie(input, output) = open_mapped_input(active, active.value("enabled", false), input_enabled);
        }
#endif
      }
      previous_device = identity;
      previous_generation = current_generation;
      for (int attempt = 0; attempt < 10 && !stop.stop_requested(); ++attempt) {
#if defined(__linux__)
        if (input >= 0) {
          process_buttons(input, output, active);
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds {20});
      }
    }
#if defined(__linux__)
    close_mapped_input(input, output);
#endif
  }
}  // namespace steamshine_x52
