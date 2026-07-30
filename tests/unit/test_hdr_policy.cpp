/**
 * @file tests/unit/test_hdr_policy.cpp
 * @brief Unit tests for deterministic HDR10 policy evaluation.
 */

#include "src/hdr_policy.h"

#include <gtest/gtest.h>

namespace {
  /**
   * @brief Return a complete known-good HDR gate set.
   *
   * @return Capability gates that select HDR10.
   */
  constexpr hdr_policy::gates_t supported_gates() {
    return {true, true, true, true, true, true, true, true, true};
  }
}  // namespace

/**
 * @brief Verify that only documented policy spellings are accepted.
 */
TEST(HdrPolicyTest, ParsesOnlyDocumentedValues) {
  EXPECT_EQ(hdr_policy::parse_policy("off"), hdr_policy::policy_e::off);
  EXPECT_EQ(hdr_policy::parse_policy("auto"), hdr_policy::policy_e::auto_select);
  EXPECT_EQ(hdr_policy::parse_policy("require"), hdr_policy::policy_e::require);
  EXPECT_FALSE(hdr_policy::parse_policy("automatic"));
  EXPECT_EQ(hdr_policy::to_string(hdr_policy::policy_e::off), "off");
  EXPECT_EQ(hdr_policy::to_string(hdr_policy::policy_e::auto_select), "auto");
  EXPECT_EQ(hdr_policy::to_string(hdr_policy::policy_e::require), "require");
}

/**
 * @brief Verify off and non-requested sessions remain explicitly SDR.
 */
TEST(HdrPolicyTest, KeepsSdrSelectionExplicit) {
  const auto disabled {hdr_policy::evaluate(hdr_policy::policy_e::off, supported_gates())};
  EXPECT_TRUE(disabled.accepted);
  EXPECT_FALSE(disabled.selected);
  EXPECT_EQ(disabled.bit_depth, 8u);
  EXPECT_EQ(disabled.reason, "hdr_disabled_by_policy");

  auto gates {supported_gates()};
  gates.client_requested = false;
  const auto automatic {hdr_policy::evaluate(hdr_policy::policy_e::auto_select, gates)};
  EXPECT_TRUE(automatic.accepted);
  EXPECT_FALSE(automatic.selected);
  EXPECT_EQ(automatic.reason, "hdr_not_requested");

  const auto required {hdr_policy::evaluate(hdr_policy::policy_e::require, gates)};
  EXPECT_FALSE(required.accepted);
  EXPECT_EQ(required.reason, "hdr_required_but_not_requested");
}

/**
 * @brief Verify every complete HDR gate selects a 10-bit transaction.
 */
TEST(HdrPolicyTest, SelectsHdrOnlyWhenEveryGatePasses) {
  const auto result {hdr_policy::evaluate(hdr_policy::policy_e::auto_select, supported_gates())};
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.selected);
  EXPECT_EQ(result.bit_depth, 10u);
  EXPECT_EQ(result.reason, "hdr_ready");

  const auto required {hdr_policy::evaluate(hdr_policy::policy_e::require, supported_gates())};
  EXPECT_TRUE(required.accepted);
  EXPECT_TRUE(required.selected);
  EXPECT_EQ(required.bit_depth, 10u);
}

/**
 * @brief Verify auto mode safely falls back and required mode rejects each gate.
 */
TEST(HdrPolicyTest, ReportsEachFailedGate) {
  struct case_t {
    bool hdr_policy::gates_t::*field;  ///< Capability field disabled for this case.
    std::string_view reason;  ///< Expected stable failure reason.
  };

  constexpr case_t cases[] {
    {&hdr_policy::gates_t::client_capable, "hdr_client_not_capable"},
    {&hdr_policy::gates_t::source_capable, "hdr_source_not_capable"},
    {&hdr_policy::gates_t::display_active, "hdr_display_not_active"},
    {&hdr_policy::gates_t::capture_10bit, "hdr_capture_not_10bit"},
    {&hdr_policy::gates_t::capture_metadata, "hdr_capture_metadata_missing"},
    {&hdr_policy::gates_t::conversion_10bit, "hdr_conversion_not_10bit"},
    {&hdr_policy::gates_t::encoder_10bit, "hdr_encoder_profile_unavailable"},
    {&hdr_policy::gates_t::signaling_available, "hdr_signaling_unavailable"},
  };

  for (const auto &test_case : cases) {
    auto gates {supported_gates()};
    gates.*(test_case.field) = false;
    const auto automatic {hdr_policy::evaluate(hdr_policy::policy_e::auto_select, gates)};
    EXPECT_TRUE(automatic.accepted);
    EXPECT_FALSE(automatic.selected);
    EXPECT_EQ(automatic.bit_depth, 8u);
    EXPECT_EQ(automatic.reason, test_case.reason);

    const auto required {hdr_policy::evaluate(hdr_policy::policy_e::require, gates)};
    EXPECT_FALSE(required.accepted);
    EXPECT_FALSE(required.selected);
    EXPECT_EQ(required.reason, test_case.reason);
  }
}
