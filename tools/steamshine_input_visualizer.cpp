/**
 * @file tools/steamshine_input_visualizer.cpp
 * @brief Fullscreen, disk-free visual probe for end-to-end streaming input latency.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/joystick.h>
#include <poll.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace {
  constexpr std::chrono::milliseconds frame_interval {16};  ///< Approximate 60 Hz repaint interval.

  /**
   * @brief Return the Linux raw monotonic clock in nanoseconds.
   *
   * @return Nanoseconds from `CLOCK_MONOTONIC_RAW`, or zero on failure.
   */
  std::uint64_t raw_monotonic_nanoseconds() {
    timespec timestamp {};
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0) {
      return 0;
    }
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(timestamp.tv_nsec);
  }

  /**
   * @brief In-memory state rendered into every diagnostic frame.
   */
  struct visual_state_t {
    std::uint64_t frame_counter {0};  ///< Number of completed repaints.
    std::uint64_t event_sequence {0};  ///< Number of observed input events.
    std::uint64_t event_time_nanoseconds {0};  ///< Raw monotonic time of the newest event.
    std::size_t color_index {0};  ///< Palette entry selected by the newest event.
    std::string event_name {"waiting"};  ///< Short label for the newest event class.

    /**
     * @brief Record an event entirely in RAM and select the next screen color.
     *
     * @param name Stable event-class label.
     */
    void record(const std::string &name) {
      ++event_sequence;
      event_time_nanoseconds = raw_monotonic_nanoseconds();
      color_index = static_cast<std::size_t>(event_sequence % 6);
      event_name = name;
    }
  };

  /**
   * @brief Open readable Linux joystick devices without changing their ownership or grab state.
   *
   * @return Nonblocking joystick file descriptors.
   */
  std::vector<int> open_joysticks() {
    std::vector<int> descriptors;
    for (int index {}; index < 16; ++index) {
      const auto path {"/dev/input/js" + std::to_string(index)};
      const int descriptor {::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
      if (descriptor >= 0) {
        descriptors.emplace_back(descriptor);
      }
    }
    return descriptors;
  }

  /**
   * @brief Close all joystick descriptors opened by @ref open_joysticks.
   *
   * @param descriptors Descriptors to close.
   */
  void close_joysticks(const std::vector<int> &descriptors) {
    for (const int descriptor : descriptors) {
      ::close(descriptor);
    }
  }

  /**
   * @brief Request EWMH fullscreen state for the visualizer window.
   *
   * @param display X11 display connection.
   * @param window Visualizer window.
   */
  void request_fullscreen(Display *display, const Window window) {
    const Atom state {XInternAtom(display, "_NET_WM_STATE", False)};
    const Atom fullscreen {XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False)};
    XChangeProperty(display, window, state, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(&fullscreen), 1);
  }

  /**
   * @brief Enable XInput2 pointer and touch events when the server supports them.
   *
   * @param display X11 display connection.
   * @param window Visualizer window.
   * @param opcode Receives the XInput extension opcode.
   * @return True when XInput2 event selection succeeded.
   */
  bool select_xinput_events(Display *display, const Window window, int &opcode) {
    int event_base {};
    int error_base {};
    if (!XQueryExtension(display, "XInputExtension", &opcode, &event_base, &error_base)) {
      return false;
    }
    int major {2};
    int minor {2};
    if (XIQueryVersion(display, &major, &minor) != Success) {
      return false;
    }
    std::array<unsigned char, (XI_LASTEVENT + 7) / 8> bits {};
    XISetMask(bits.data(), XI_Motion);
    XISetMask(bits.data(), XI_ButtonPress);
    XISetMask(bits.data(), XI_ButtonRelease);
    XISetMask(bits.data(), XI_KeyPress);
    XISetMask(bits.data(), XI_KeyRelease);
    XISetMask(bits.data(), XI_TouchBegin);
    XISetMask(bits.data(), XI_TouchUpdate);
    XISetMask(bits.data(), XI_TouchEnd);
    XIEventMask mask {
      .deviceid = XIAllMasterDevices,
      .mask_len = static_cast<int>(bits.size()),
      .mask = bits.data(),
    };
    return XISelectEvents(display, window, &mask, 1) == Success;
  }

  /**
   * @brief Convert an XInput2 event type to a stable visual label.
   *
   * @param event_type XInput2 event identifier.
   * @return Label, or an empty string for an unrelated event.
   */
  std::string xinput_event_name(const int event_type) {
    switch (event_type) {
      case XI_Motion:
        return "pointer-motion";
      case XI_ButtonPress:
        return "pointer-button-down";
      case XI_ButtonRelease:
        return "pointer-button-up";
      case XI_KeyPress:
        return "key-down";
      case XI_KeyRelease:
        return "key-up";
      case XI_TouchBegin:
        return "touch-begin";
      case XI_TouchUpdate:
        return "touch-move";
      case XI_TouchEnd:
        return "touch-end";
      default:
        return {};
    }
  }

  /**
   * @brief Draw the current counters and event timestamp over a solid color.
   *
   * @param display X11 display connection.
   * @param window Visualizer window.
   * @param graphics Graphics context used for fill and text.
   * @param colors Preallocated diagnostic palette.
   * @param state Current in-memory visual state.
   */
  void draw_frame(Display *display, const Window window, const GC graphics, const std::array<unsigned long, 6> &colors, visual_state_t &state) {
    XWindowAttributes attributes {};
    XGetWindowAttributes(display, window, &attributes);
    XSetForeground(display, graphics, colors[state.color_index]);
    XFillRectangle(display, window, graphics, 0, 0, static_cast<unsigned int>(attributes.width), static_cast<unsigned int>(attributes.height));
    XSetForeground(display, graphics, BlackPixel(display, DefaultScreen(display)));

    const std::array<std::string, 4> lines {
      "SteamShine Input Visualizer",
      "frame=" + std::to_string(state.frame_counter),
      "event_sequence=" + std::to_string(state.event_sequence) + " event=" + state.event_name,
      "event_CLOCK_MONOTONIC_RAW_ns=" + std::to_string(state.event_time_nanoseconds),
    };
    int y {80};
    for (const auto &line : lines) {
      XDrawString(display, window, graphics, 60, y, line.c_str(), static_cast<int>(line.size()));
      y += 42;
    }
    XFlush(display);
    ++state.frame_counter;
  }

  /**
   * @brief Execute a display-independent smoke test for CI.
   *
   * @return Zero when sequence and raw-clock state advance correctly.
   */
  int self_test() {
    visual_state_t state;
    state.record("key-down");
    const auto first_time {state.event_time_nanoseconds};
    state.record("touch-move");
    if (state.event_sequence != 2 || state.event_name != "touch-move" || state.event_time_nanoseconds < first_time) {
      return 1;
    }
    std::cout << "steamshine-input-visualizer self-test passed\n";
    return 0;
  }
}  // namespace

/**
 * @brief Run the fullscreen input visualizer or its display-independent self-test.
 *
 * @param argc Argument count.
 * @param argv Argument vector; `--self-test` avoids opening a display.
 * @return Zero after a clean exit, nonzero when initialization fails.
 */
int main(const int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
    return self_test();
  }

  Display *const display {XOpenDisplay(nullptr)};
  if (!display) {
    std::cerr << "Unable to open DISPLAY\n";
    return 1;
  }
  const int screen {DefaultScreen(display)};
  const Window root {RootWindow(display, screen)};
  const Window window {XCreateSimpleWindow(display, root, 0, 0, 1280, 720, 0, BlackPixel(display, screen), WhitePixel(display, screen))};
  XStoreName(display, window, "SteamShine Input Visualizer");
  XSelectInput(display, window, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
  int xinput_opcode {-1};
  (void) select_xinput_events(display, window, xinput_opcode);
  request_fullscreen(display, window);
  XMapRaised(display, window);

  const GC graphics {XCreateGC(display, window, 0, nullptr)};
  const std::array<std::string, 6> color_names {"#e74c3c", "#2ecc71", "#3498db", "#f1c40f", "#9b59b6", "#1abc9c"};
  std::array<unsigned long, 6> colors {};
  const Colormap colormap {DefaultColormap(display, screen)};
  for (std::size_t index {}; index < colors.size(); ++index) {
    XColor color {};
    XParseColor(display, colormap, color_names[index].c_str(), &color);
    XAllocColor(display, colormap, &color);
    colors[index] = color.pixel;
  }

  auto joysticks {open_joysticks()};
  visual_state_t state;
  bool running {true};
  auto next_frame {std::chrono::steady_clock::now()};
  while (running) {
    std::vector<pollfd> descriptors;
    descriptors.reserve(joysticks.size() + 1);
    descriptors.push_back({.fd = ConnectionNumber(display), .events = POLLIN, .revents = 0});
    for (const int joystick : joysticks) {
      descriptors.push_back({.fd = joystick, .events = POLLIN, .revents = 0});
    }
    const auto until_frame {std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - std::chrono::steady_clock::now()).count()};
    (void) ::poll(descriptors.data(), descriptors.size(), static_cast<int>(std::max<std::int64_t>(until_frame, 0)));

    while (XPending(display) > 0) {
      XEvent event {};
      XNextEvent(display, &event);
      if (event.type == KeyPress) {
        if (XLookupKeysym(&event.xkey, 0) == XK_Escape) {
          running = false;
        }
        state.record("key-down");
      } else if (event.type == KeyRelease) {
        state.record("key-up");
      } else if (event.type == ButtonPress) {
        state.record("pointer-button-down");
      } else if (event.type == ButtonRelease) {
        state.record("pointer-button-up");
      } else if (event.type == MotionNotify) {
        state.record("pointer-motion");
      } else if (event.type == GenericEvent && event.xcookie.extension == xinput_opcode && XGetEventData(display, &event.xcookie)) {
        const auto name {xinput_event_name(event.xcookie.evtype)};
        if (!name.empty()) {
          state.record(name);
        }
        XFreeEventData(display, &event.xcookie);
      }
    }

    for (std::size_t index {}; index < joysticks.size(); ++index) {
      if ((descriptors[index + 1].revents & POLLIN) == 0) {
        continue;
      }
      js_event event {};
      while (::read(joysticks[index], &event, sizeof(event)) == sizeof(event)) {
        const auto type {static_cast<unsigned char>(event.type & ~JS_EVENT_INIT)};
        state.record(type == JS_EVENT_AXIS ? "controller-axis" : "controller-button");
      }
    }

    const auto now {std::chrono::steady_clock::now()};
    if (now >= next_frame) {
      draw_frame(display, window, graphics, colors, state);
      next_frame = now + frame_interval;
    }
  }

  close_joysticks(joysticks);
  XFreeGC(display, graphics);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return 0;
}
