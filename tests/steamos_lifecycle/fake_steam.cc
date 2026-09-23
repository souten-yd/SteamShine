/**
 * @file tests/steamos_lifecycle/fake_steam.cc
 * @brief Minimal Steam lifecycle fixture for owned-Gamescope shutdown tests.
 */

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
  /**
   * @brief Read a decimal process identifier from a fixture file.
   *
   * @param path File containing one decimal PID.
   * @return Parsed PID, or `-1` when unavailable.
   */
  pid_t read_pid(const std::filesystem::path &path) {
    std::ifstream input {path};
    pid_t pid {-1};
    input >> pid;
    return pid;
  }
}  // namespace

/**
 * @brief Emulate the resident and `-shutdown` modes of the Steam executable.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero after a shutdown request, otherwise waits as the resident.
 */
int main(const int argc, char *argv[]) {
  const char *runtime_value {std::getenv("XDG_RUNTIME_DIR")};
  if (!runtime_value) {
    return 2;
  }
  const std::filesystem::path runtime {runtime_value};
  const auto resident_pid_file {runtime / "fake-steam.pid"};
  if (argc > 1 && std::string {argv[1]} == "-shutdown") {
    const auto gamescope_pid {read_pid(runtime / "gamescope.pid")};
    const bool gamescope_alive {gamescope_pid > 0 && (::kill(gamescope_pid, 0) == 0 || errno == EPERM)};
    std::ofstream marker {runtime.parent_path().parent_path() / "fake-steam-shutdown"};
    marker << "gamescope_alive=" << (gamescope_alive ? "true" : "false");
    const auto resident_pid {read_pid(resident_pid_file)};
    if (resident_pid > 0) {
      ::kill(resident_pid, SIGTERM);
    }
    return 0;
  }

  std::ofstream {resident_pid_file} << ::getpid();
  std::signal(SIGTERM, [](int) {
    std::_Exit(0);
  });
  while (true) {
    ::pause();
  }
}
