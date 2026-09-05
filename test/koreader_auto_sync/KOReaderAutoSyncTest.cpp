#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/KOReaderSync/KOReaderAutoSyncSetting.h"
#include "lib/KOReaderSync/KOReaderSyncSession.h"

namespace {

using Comparison = KOReaderSyncComparison;
using Pending = KOReaderSyncPendingTransition;

struct FakeTransport final : KOReaderSyncTransport {
  uint32_t clockMs = 0;
  KOReaderSyncClient::Error getResult = KOReaderSyncClient::OK;
  KOReaderSyncClient::Error putResult = KOReaderSyncClient::OK;
  KOReaderProgress remote;
  std::vector<std::string> calls;
  std::string fetchedHash;
  std::string uploadedDocument;
  uint32_t getTimeout = 0;
  uint32_t putTimeout = 0;

  uint32_t nowMs() const override { return clockMs; }

  KOReaderSyncClient::Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress,
                                        uint32_t timeoutMs) override {
    calls.emplace_back("GET");
    fetchedHash = documentHash;
    getTimeout = timeoutMs;
    outProgress = remote;
    return getResult;
  }

  KOReaderSyncClient::Error updateProgress(const KOReaderProgress& progress, uint32_t timeoutMs) override {
    calls.emplace_back("PUT");
    uploadedDocument = progress.document;
    putTimeout = timeoutMs;
    return putResult;
  }
};

KOReaderSyncSnapshot localSnapshot(const float percentage = 0.8f) {
  KOReaderSyncSnapshot snapshot;
  snapshot.documentHash = "configured-hash";
  snapshot.localProgress.document = snapshot.documentHash;
  snapshot.localProgress.progress = "/local";
  snapshot.localProgress.percentage = percentage;
  return snapshot;
}

TEST(KOReaderAutoSync, DecisionTableForAskAndSmart) {
  const Comparison comparisons[] = {Comparison::Equal, Comparison::LocalAhead, Comparison::RemoteAhead,
                                   Comparison::Missing, Comparison::Unknown};
  for (const auto comparison : comparisons) {
    EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Open, KOReaderSyncMode::Ask, comparison),
              comparison == Comparison::Equal ? Pending::None : Pending::Prompt);
    const auto smartExpected = comparison == Comparison::Equal     ? Pending::None
                               : comparison == Comparison::LocalAhead || comparison == Comparison::Missing
                                   ? Pending::UploadLocal
                               : comparison == Comparison::RemoteAhead ? Pending::ApplyRemote
                                                                        : Pending::Prompt;
    EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::BookSwitch, KOReaderSyncMode::Smart, comparison),
              smartExpected);
  }
}

TEST(KOReaderAutoSync, PersistedSettingIsFalseForMissingOrInvalidOnBothSurfaces) {
  EXPECT_FALSE(KOReaderAutoSyncSetting::fromStoredValue(false, true));
  EXPECT_FALSE(KOReaderAutoSyncSetting::fromStoredValue(false, false));
  EXPECT_FALSE(KOReaderAutoSyncSetting::fromStoredValue(true, false));
  EXPECT_TRUE(KOReaderAutoSyncSetting::fromStoredValue(true, true));
}

TEST(KOReaderAutoSync, AutomaticSmartUploadUsesOneConfiguredHashAndOneTransportSequence) {
  FakeTransport transport;
  transport.remote.percentage = 0.5f;
  KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Open, KOReaderSyncMode::Smart, 0, transport);

  const auto fetched = session.run(0);
  ASSERT_EQ(fetched.error, KOReaderSyncClient::OK);
  ASSERT_TRUE(fetched.hasRemoteProgress);
  const auto outcome = session.resolve(Comparison::LocalAhead, transport.nowMs());

  ASSERT_EQ(outcome.error, KOReaderSyncClient::OK);
  EXPECT_EQ(outcome.pending, Pending::None);
  ASSERT_EQ(transport.calls, (std::vector<std::string>{"GET", "PUT"}));
  EXPECT_EQ(transport.fetchedHash, "configured-hash");
  EXPECT_EQ(transport.uploadedDocument, "configured-hash");
  EXPECT_EQ(transport.getTimeout, KOReaderSyncSession::DEADLINE_MS);
  EXPECT_EQ(transport.putTimeout, KOReaderSyncSession::DEADLINE_MS);
}

TEST(KOReaderAutoSync, AskLeavesExistingRemoteForThePresenter) {
  FakeTransport transport;
  transport.remote.percentage = 0.5f;
  KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Open, KOReaderSyncMode::Ask, 0, transport);

  const auto fetched = session.run(0);
  ASSERT_TRUE(fetched.hasRemoteProgress);
  const auto outcome = session.resolve(Comparison::LocalAhead, transport.nowMs());
  ASSERT_EQ(outcome.pending, Pending::Prompt);
  EXPECT_EQ(transport.calls, (std::vector<std::string>{"GET"}));
  EXPECT_EQ(transport.fetchedHash, "configured-hash");
}

TEST(KOReaderAutoSync, SleepUploadsOnlyMissingOrKnownLocalAheadAndAlwaysContinues) {
  {
    FakeTransport transport;
    transport.getResult = KOReaderSyncClient::NOT_FOUND;
    KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Ask, 0, transport);
    const auto outcome = session.run(0);
    EXPECT_EQ(outcome.pending, Pending::ContinueSleep);
    EXPECT_EQ(transport.calls, (std::vector<std::string>{"GET", "PUT"}));
  }
  {
    FakeTransport transport;
    transport.remote.percentage = 0.5f;
    KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, 0, transport);
    const auto fetched = session.run(0);
    ASSERT_TRUE(fetched.hasRemoteProgress);
    const auto outcome = session.resolve(Comparison::LocalAhead, transport.nowMs());
    EXPECT_EQ(outcome.pending, Pending::ContinueSleep);
    EXPECT_EQ(transport.calls, (std::vector<std::string>{"GET", "PUT"}));
  }
  for (const auto comparison : {Comparison::RemoteAhead, Comparison::Unknown}) {
    FakeTransport transport;
    transport.remote.percentage = 0.9f;
    KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, 0, transport);
    const auto fetched = session.run(0);
    ASSERT_TRUE(fetched.hasRemoteProgress);
    const auto outcome = session.resolve(comparison, transport.nowMs());
    EXPECT_EQ(outcome.pending, Pending::ContinueSleep);
    EXPECT_EQ(transport.calls, (std::vector<std::string>{"GET"}));
  }
  {
    FakeTransport transport;
    transport.getResult = KOReaderSyncClient::NETWORK_ERROR;
    KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, 0, transport);
    const auto outcome = session.run(0);
    EXPECT_EQ(outcome.pending, Pending::ContinueSleep);
    EXPECT_EQ(transport.calls, (std::vector<std::string>{"GET"}));
  }
  {
    FakeTransport transport;
    KOReaderSyncSession session(localSnapshot(), KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, 0, transport);
    const auto outcome = session.run(KOReaderSyncSession::DEADLINE_MS);
    EXPECT_EQ(outcome.pending, Pending::ContinueSleep);
    EXPECT_TRUE(transport.calls.empty());
  }
}

TEST(KOReaderAutoSync, ManualTriggerRemainsAnExplicitNonSleepFlow) {
  EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Manual, KOReaderSyncMode::Ask, Comparison::RemoteAhead),
            Pending::Prompt);
}

TEST(KOReaderAutoSync, AutomaticWifiCleanupOnlyReleasesOwnedConnection) {
  EXPECT_TRUE(KOReaderSyncSession::shouldDisconnectAutomaticWifi(false, true));
  EXPECT_FALSE(KOReaderSyncSession::shouldDisconnectAutomaticWifi(true, true));
  EXPECT_FALSE(KOReaderSyncSession::shouldDisconnectAutomaticWifi(false, false));
}

TEST(KOReaderAutoSync, ManualAndAutomaticReturnsSuppressImmediateOpenReentry) {
  EXPECT_TRUE(KOReaderSyncSession::suppressImmediateOpen(KOReaderSyncTrigger::Manual));
  EXPECT_TRUE(KOReaderSyncSession::suppressImmediateOpen(KOReaderSyncTrigger::Open));
  EXPECT_TRUE(KOReaderSyncSession::suppressImmediateOpen(KOReaderSyncTrigger::Wake));
  EXPECT_FALSE(KOReaderSyncSession::suppressImmediateOpen(KOReaderSyncTrigger::Sleep));
}

TEST(KOReaderAutoSync, SleepAlwaysContinuesAndNeverAppliesRemote) {
  for (const auto comparison : {Comparison::Equal, Comparison::RemoteAhead, Comparison::Unknown}) {
    EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, comparison),
              Pending::ContinueSleep);
  }
  EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, Comparison::LocalAhead),
            Pending::UploadLocal);
  EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, Comparison::Missing),
            Pending::UploadLocal);
}

TEST(KOReaderAutoSync, UnknownComparisonCannotAuthorizeAnAutomaticWrite) {
  EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Open, KOReaderSyncMode::Smart, Comparison::Unknown),
            Pending::Prompt);
  EXPECT_EQ(KOReaderSyncSession::decide(KOReaderSyncTrigger::Sleep, KOReaderSyncMode::Smart, Comparison::Unknown),
            Pending::ContinueSleep);
}

TEST(KOReaderAutoSync, DeadlineUsesUnsignedWrapSafeElapsedTime) {
  constexpr uint32_t start = UINT32_MAX - 1000U;
  EXPECT_EQ(KOReaderSyncSession::remainingMs(start + 500U, start), 14500U);
  EXPECT_TRUE(KOReaderSyncSession::deadlineExpired(start + 15000U, start));
}

}  // namespace
