/**
 * @file tests/unit/test_web_services.cpp
 * @brief Tests for pure shared Web service validation.
 */

// test imports
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <map>
#include <memory>

// local imports
#include <src/config.h>
#include <src/crypto.h>
#include <src/utility.h>
#include <src/web_services.h>

namespace {
  /**
   * @brief In-memory paired-client backend for shared-service tests.
   */
  class FakePairingClientBackend final: public web::PairingClientBackend {
  public:
    /** @copydoc web::PairingClientBackend::submit_pin */
    bool submit_pin(const std::string_view pin, const std::string_view client_name) override {
      if (pin != "1234") {
        return false;
      }
      clients_.emplace("test-client", nlohmann::json {{"uuid", "test-client"}, {"name", client_name}});
      return true;
    }

    /** @copydoc web::PairingClientBackend::list_clients */
    nlohmann::json list_clients() const override {
      nlohmann::json clients = nlohmann::json::array();
      for (const auto &entry : clients_) {
        clients.push_back(entry.second);
      }
      return clients;
    }

    /** @copydoc web::PairingClientBackend::certificate_for_uuid */
    std::string certificate_for_uuid(const std::string_view uuid) const override {
      return clients_.contains(std::string {uuid}) ? "test-certificate" : "";
    }

    /** @copydoc web::PairingClientBackend::revoke_client */
    bool revoke_client(const std::string_view uuid) override {
      return clients_.erase(std::string {uuid}) > 0;
    }

  private:
    std::map<std::string, nlohmann::json, std::less<>> clients_;  ///< Paired clients indexed by stable test UUID.
  };
}  // namespace

/**
 * @brief Validate the exact Moonlight PIN boundary accepted by Web services.
 */
TEST(WebServicesTest, ValidatesFourDigitPins) {
  EXPECT_TRUE(web::is_valid_pin("0000"));
  EXPECT_TRUE(web::is_valid_pin("1234"));
  EXPECT_FALSE(web::is_valid_pin("123"));
  EXPECT_FALSE(web::is_valid_pin("12345"));
  EXPECT_FALSE(web::is_valid_pin("12a4"));
  EXPECT_FALSE(web::is_valid_pin("１２３４"));
}

/**
 * @brief Verify that logout and global invalidation immediately revoke sessions.
 */
TEST(WebServicesTest, InvalidatesSessions) {
  const auto original_username = config::sunshine.username;
  const auto original_password = config::sunshine.password;
  const auto original_salt = config::sunshine.salt;
  config::sunshine.username = "web-services-test";
  config::sunshine.salt = "web-services-test-salt";
  config::sunshine.password = util::hex(crypto::hash("web-services-test-password" + config::sunshine.salt)).to_string();

  web::CredentialService credentials;
  web::SessionService sessions;
  const auto first_session = sessions.login(credentials, "web-services-test", "web-services-test-password");
  ASSERT_TRUE(first_session.has_value());
  EXPECT_TRUE(sessions.validate(first_session->id).has_value());
  EXPECT_TRUE(sessions.validate_csrf(first_session->id, first_session->csrf_token));
  EXPECT_FALSE(sessions.validate_csrf(first_session->id, "invalid-token"));

  sessions.logout(first_session->id);
  EXPECT_FALSE(sessions.validate(first_session->id).has_value());

  const auto second_session = sessions.login(credentials, "web-services-test", "web-services-test-password");
  ASSERT_TRUE(second_session.has_value());
  sessions.invalidate_all();
  EXPECT_FALSE(sessions.validate(second_session->id).has_value());

  config::sunshine.username = original_username;
  config::sunshine.password = original_password;
  config::sunshine.salt = original_salt;
}

/**
 * @brief Verify that expired sessions are purged before they can authorize a request.
 */
TEST(WebServicesTest, RejectsExpiredSessions) {
  const auto original_username = config::sunshine.username;
  const auto original_password = config::sunshine.password;
  const auto original_salt = config::sunshine.salt;
  config::sunshine.username = "web-services-test";
  config::sunshine.salt = "web-services-test-salt";
  config::sunshine.password = util::hex(crypto::hash("web-services-test-password" + config::sunshine.salt)).to_string();

  web::CredentialService credentials;
  web::SessionService sessions {std::chrono::seconds::zero()};
  const auto session = sessions.login(credentials, "web-services-test", "web-services-test-password");
  ASSERT_TRUE(session.has_value());
  EXPECT_FALSE(sessions.validate(session->id).has_value());

  config::sunshine.username = original_username;
  config::sunshine.password = original_password;
  config::sunshine.salt = original_salt;
}

/**
 * @brief Verify pairing and revocation share one client state backend.
 */
TEST(WebServicesTest, SharesPairingAndClientState) {
  const auto backend = std::make_shared<FakePairingClientBackend>();
  web::PairingService pairing {backend};
  web::ClientService clients {backend};

  EXPECT_FALSE(pairing.submit_pin("0000", "Moonlight test").success);
  EXPECT_TRUE(clients.list().empty());

  EXPECT_TRUE(pairing.submit_pin("1234", "Moonlight test").success);
  const auto paired_clients = clients.list();
  ASSERT_EQ(paired_clients.size(), 1);
  EXPECT_EQ(paired_clients.at(0).at("name"), "Moonlight test");

  EXPECT_TRUE(clients.revoke("test-client").success);
  EXPECT_TRUE(clients.list().empty());
  EXPECT_FALSE(clients.revoke("test-client").success);
}
