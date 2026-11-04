/**
 * @file tests/unit/test_pairing_retry.cpp
 * @brief Regression tests for replacing abandoned pairing handshakes.
 */
#include "../tests_common.h"
#include "src/nvhttp.h"

namespace nvhttp {
  /** @brief Insert a fresh attempt or preserve one still waiting for PIN approval. */
  pair_session_t &insert_pair_session(pair_session_t &&session);
  /** @brief Add or replace a paired certificate. */
  void add_authorized_client(const std::string &name, std::string &&cert);
  /** @brief Execute assertions without touching persistent client data. */
  void test_with_isolated_clients(const std::function<void()> &test);
}  // namespace nvhttp

TEST(PairingCertificateTest, ReplacesExactIdentityAndKeepsDistinctCertificates) {
  const auto first = crypto::gen_creds("client", 2048);
  const auto second = crypto::gen_creds("client", 2048);
  nvhttp::test_with_isolated_clients([&]() {
    nvhttp::add_authorized_client("first", std::string(first.x509));
    nvhttp::add_authorized_client("other", std::string(second.x509));
    const auto before = nvhttp::get_all_clients();
    ASSERT_EQ(before.size(), 2);
    nvhttp::add_authorized_client("replacement", std::string(first.x509));
    auto after = nvhttp::get_all_clients();
    ASSERT_EQ(after.size(), 2);
    EXPECT_EQ(after[0]["name"], "other");
    EXPECT_EQ(after[0]["uuid"], before[1]["uuid"]);
    EXPECT_EQ(after[1]["name"], "replacement");
    EXPECT_NE(after[1]["uuid"], before[0]["uuid"]);
    nvhttp::add_authorized_client("invalid", "not a certificate");
    EXPECT_EQ(nvhttp::get_all_clients(), after);
  });
}

TEST(PairingRetryTest, KeepsPendingApprovalAndReplacesEveryStartedPhase) {
  const std::string id = "test-pairing-retry";
  nvhttp::pair_session_t initial;
  initial.client.uniqueID = id;
  initial.client.cert = "original";
  auto &pending = nvhttp::insert_pair_session(std::move(initial));
  auto cleanup = util::fail_guard([&]() {
    nvhttp::pair_session_t key;
    key.client.uniqueID = id;
    nvhttp::remove_session(key);
  });
  nvhttp::pair_session_t duplicate;
  duplicate.client.uniqueID = id;
  duplicate.client.cert = "replacement";
  EXPECT_EQ(&pending, &nvhttp::insert_pair_session(std::move(duplicate)));
  EXPECT_EQ(pending.client.cert, "original");
  for (const auto phase : {nvhttp::PAIR_PHASE::GETSERVERCERT, nvhttp::PAIR_PHASE::CLIENTCHALLENGE, nvhttp::PAIR_PHASE::SERVERCHALLENGERESP, nvhttp::PAIR_PHASE::CLIENTPAIRINGSECRET}) {
    nvhttp::pair_session_t lookup;
    lookup.client.uniqueID = id;
    auto &old = nvhttp::insert_pair_session(std::move(lookup));
    old.last_phase = phase;
    old.serversecret = "stale secret";
    nvhttp::pair_session_t retry;
    retry.client.uniqueID = id;
    retry.client.cert = "new certificate";
    const auto &fresh = nvhttp::insert_pair_session(std::move(retry));
    EXPECT_EQ(fresh.last_phase, nvhttp::PAIR_PHASE::NONE);
    EXPECT_EQ(fresh.client.cert, "new certificate");
    EXPECT_TRUE(fresh.serversecret.empty());
  }
}
