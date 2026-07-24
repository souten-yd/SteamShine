/**
 * @file tests/steamos_lifecycle/stubs.cpp
 * @brief Minimal runtime globals required by standalone lifecycle tests.
 */
#include "src/config.h"
#include "src/logging.h"

namespace config {
  steamos_virtual_display_t steamos_virtual_display {};  ///< Test-owned virtual-display configuration.
}

boost::log::sources::severity_logger<int> verbose;  ///< Standalone verbose test logger.
boost::log::sources::severity_logger<int> debug;  ///< Standalone debug test logger.
boost::log::sources::severity_logger<int> info;  ///< Standalone info test logger.
boost::log::sources::severity_logger<int> warning;  ///< Standalone warning test logger.
boost::log::sources::severity_logger<int> error;  ///< Standalone error test logger.
boost::log::sources::severity_logger<int> fatal;  ///< Standalone fatal test logger.
#ifdef SUNSHINE_TESTS
boost::log::sources::severity_logger<int> tests;  ///< Standalone test logger.
#endif
