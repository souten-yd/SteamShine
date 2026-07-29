/**
 * @file tools/steamshine_input_visualizer.cpp
 * @brief Fullscreen, disk-free visual probe for end-to-end streaming input latency.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <EGL/egl.h>
#include <fcntl.h>
#include <GLES2/gl2.h>
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
#include <X11/Xutil.h>

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
   * @brief Draw an animated diagnostic color through the GPU swapchain.
   *
   * @param state Current in-memory visual state.
   */
  void draw_frame(visual_state_t &state) {
    constexpr std::array<std::array<GLfloat, 3>, 6> colors {{
      {0.91F, 0.30F, 0.24F},
      {0.18F, 0.80F, 0.44F},
      {0.20F, 0.60F, 0.86F},
      {0.95F, 0.77F, 0.06F},
      {0.61F, 0.35F, 0.71F},
      {0.10F, 0.74F, 0.61F},
    }};
    const auto &color {colors[state.color_index]};
    const GLfloat pulse {0.65F + 0.35F * static_cast<GLfloat>(state.frame_counter % 60) / 59.0F};
    glClearColor(color[0] * pulse, color[1] * pulse, color[2] * pulse, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
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

  const EGLDisplay egl_display {eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display))};
  EGLint egl_major {};
  EGLint egl_minor {};
  if (egl_display == EGL_NO_DISPLAY || eglInitialize(egl_display, &egl_major, &egl_minor) != EGL_TRUE || eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    std::cerr << "Unable to initialize EGL\n";
    XCloseDisplay(display);
    return 1;
  }

  constexpr std::array<EGLint, 13> config_attributes {
    EGL_SURFACE_TYPE,
    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,
    8,
    EGL_GREEN_SIZE,
    8,
    EGL_BLUE_SIZE,
    8,
    EGL_ALPHA_SIZE,
    8,
    EGL_NONE,
  };
  EGLConfig egl_config {};
  EGLint config_count {};
  if (eglChooseConfig(egl_display, config_attributes.data(), &egl_config, 1, &config_count) != EGL_TRUE || config_count == 0) {
    std::cerr << "Unable to choose an EGL window configuration\n";
    eglTerminate(egl_display);
    XCloseDisplay(display);
    return 1;
  }

  EGLint visual_id {};
  if (eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &visual_id) != EGL_TRUE) {
    std::cerr << "Unable to query the EGL native visual\n";
    eglTerminate(egl_display);
    XCloseDisplay(display);
    return 1;
  }

  XVisualInfo visual_template {};
  visual_template.visualid = static_cast<VisualID>(visual_id);
  int visual_count {};
  XVisualInfo *const visual_info {XGetVisualInfo(display, VisualIDMask, &visual_template, &visual_count)};
  if (!visual_info || visual_count == 0) {
    std::cerr << "Unable to find the EGL-compatible X11 visual\n";
    if (visual_info) {
      XFree(visual_info);
    }
    eglTerminate(egl_display);
    XCloseDisplay(display);
    return 1;
  }

  const Window root {RootWindow(display, visual_info->screen)};
  const Colormap colormap {XCreateColormap(display, root, visual_info->visual, AllocNone)};
  XSetWindowAttributes window_attributes {};
  window_attributes.colormap = colormap;
  window_attributes.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
  const Window window {XCreateWindow(
    display,
    root,
    0,
    0,
    1280,
    720,
    0,
    visual_info->depth,
    InputOutput,
    visual_info->visual,
    CWColormap | CWEventMask,
    &window_attributes
  )};
  XFree(visual_info);
  XStoreName(display, window, "SteamShine Input Visualizer");
  int xinput_opcode {-1};
  (void) select_xinput_events(display, window, xinput_opcode);
  request_fullscreen(display, window);
  XMapRaised(display, window);

  const EGLSurface egl_surface {eglCreateWindowSurface(egl_display, egl_config, static_cast<EGLNativeWindowType>(window), nullptr)};
  constexpr std::array<EGLint, 3> context_attributes {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  const EGLContext egl_context {eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes.data())};
  if (egl_surface == EGL_NO_SURFACE || egl_context == EGL_NO_CONTEXT || eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
    std::cerr << "Unable to create the EGL GPU swapchain\n";
    if (egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display, egl_context);
    }
    if (egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(egl_display, egl_surface);
    }
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    eglTerminate(egl_display);
    XCloseDisplay(display);
    return 1;
  }
  (void) eglSwapInterval(egl_display, 1);
  glViewport(0, 0, 1280, 720);

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
      draw_frame(state);
      if (eglSwapBuffers(egl_display, egl_surface) != EGL_TRUE) {
        std::cerr << "EGL buffer swap failed\n";
        running = false;
      }
      next_frame = now + frame_interval;
    }
  }

  close_joysticks(joysticks);
  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(egl_display, egl_context);
  eglDestroySurface(egl_display, egl_surface);
  XDestroyWindow(display, window);
  XFreeColormap(display, colormap);
  eglTerminate(egl_display);
  XCloseDisplay(display);
  return 0;
}
