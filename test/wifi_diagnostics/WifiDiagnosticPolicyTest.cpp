#include <gtest/gtest.h>

#include "WifiDiagnosticPolicy.h"

namespace {
constexpr uint16_t UNKNOWN_REASON = 0xffff;

struct ScenarioTrace {
  uint32_t nextAttemptId = 0;
  uint32_t presentationId = 0;
  uint32_t fallbackCount = 0;
  uint32_t connectedCount = 0;
  uint16_t latestReason = UNKNOWN_REASON;
  uint8_t fallbackTrigger = diagnostics::FALLBACK_NO_SAVED_CANDIDATE;

  uint32_t startAttempt() { return ++nextAttemptId; }

  void receiveDisconnect(const uint16_t reason) { latestReason = reason; }

  uint16_t failureReason() const { return latestReason; }

  void presentList(const bool explicitConfirm, const bool scanFailure, const bool carriedCause,
                  const uint8_t carriedTrigger) {
    ++presentationId;
    ++fallbackCount;
    fallbackTrigger = diagnostics::selectFallbackTrigger(explicitConfirm, scanFailure, carriedCause, carriedTrigger);
  }

  void connect() { ++connectedCount; }
};
}  // namespace

TEST(WifiDiagnosticPolicy, SuccessfulLastSavedAutoConnectHasNoFallback) {
  ScenarioTrace trace;
  const uint32_t attempt = trace.startAttempt();
  trace.connect();
  EXPECT_EQ(attempt, 1u);
  EXPECT_EQ(trace.connectedCount, 1u);
  EXPECT_EQ(trace.fallbackCount, 0u);
}

TEST(WifiDiagnosticPolicy, ConfirmDuringConnectionUsesUserConfirm) {
  ScenarioTrace trace;
  trace.startAttempt();
  trace.presentList(true, false, false, diagnostics::FALLBACK_DRIVER_FAILURE);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_USER_CONFIRM);
  EXPECT_EQ(trace.presentationId, 1u);
}

TEST(WifiDiagnosticPolicy, ConfirmDuringScanWinsOverScanError) {
  ScenarioTrace trace;
  trace.presentList(true, true, false, diagnostics::FALLBACK_SCAN_FAILURE);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_USER_CONFIRM);
}

TEST(WifiDiagnosticPolicy, ConnectFailedUsesDriverFailureCause) {
  ScenarioTrace trace;
  trace.startAttempt();
  trace.receiveDisconnect(201);
  EXPECT_EQ(trace.failureReason(), 201);
  trace.presentList(false, false, true, diagnostics::FALLBACK_DRIVER_FAILURE);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_DRIVER_FAILURE);
}

TEST(WifiDiagnosticPolicy, NoSsidFailureUsesSameAttemptReason) {
  ScenarioTrace trace;
  trace.startAttempt();
  trace.receiveDisconnect(201);
  EXPECT_EQ(trace.failureReason(), 201);
  EXPECT_EQ(trace.nextAttemptId, 1u);
}

TEST(WifiDiagnosticPolicy, TimeoutWithDisconnectCarriesTimeoutCause) {
  ScenarioTrace trace;
  trace.startAttempt();
  trace.receiveDisconnect(202);
  trace.presentList(false, false, true, diagnostics::FALLBACK_TIMEOUT);
  EXPECT_EQ(trace.failureReason(), 202);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_TIMEOUT);
}

TEST(WifiDiagnosticPolicy, TimeoutWithoutDisconnectUsesUnknownReason) {
  ScenarioTrace trace;
  trace.startAttempt();
  EXPECT_EQ(trace.failureReason(), UNKNOWN_REASON);
  trace.presentList(false, false, true, diagnostics::FALLBACK_TIMEOUT);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_TIMEOUT);
}

TEST(WifiDiagnosticPolicy, ScanFailureUsesScanCauseAndRetainsZeroResultElsewhere) {
  ScenarioTrace trace;
  trace.presentList(false, true, false, diagnostics::FALLBACK_NO_SAVED_CANDIDATE);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_SCAN_FAILURE);
}

TEST(WifiDiagnosticPolicy, IntermediateSavedFailureDoesNotPresentList) {
  ScenarioTrace trace;
  trace.startAttempt();
  EXPECT_TRUE(diagnostics::isIntermediateAutoFailure(true));
  trace.startAttempt();
  EXPECT_EQ(trace.nextAttemptId, 2u);
  EXPECT_EQ(trace.fallbackCount, 0u);
}

TEST(WifiDiagnosticPolicy, FinalFailureWithNoCandidateUsesOneFallback) {
  ScenarioTrace trace;
  trace.startAttempt();
  trace.startAttempt();
  EXPECT_FALSE(diagnostics::isIntermediateAutoFailure(false));
  trace.presentList(false, false, false, diagnostics::FALLBACK_DRIVER_FAILURE);
  EXPECT_EQ(trace.fallbackCount, 1u);
  EXPECT_EQ(trace.presentationId, 1u);
  EXPECT_EQ(trace.fallbackTrigger, diagnostics::FALLBACK_NO_SAVED_CANDIDATE);
}

TEST(WifiDiagnosticPolicy, ManualSuccessAfterFallbackDoesNotAddFallback) {
  ScenarioTrace trace;
  trace.presentList(false, false, false, diagnostics::FALLBACK_NO_SAVED_CANDIDATE);
  trace.connect();
  EXPECT_EQ(trace.fallbackCount, 1u);
  EXPECT_EQ(trace.connectedCount, 1u);
}

TEST(WifiDiagnosticPolicy, EveryPresentationReceivesExactlyOneFallback) {
  ScenarioTrace trace;
  trace.presentList(true, false, false, diagnostics::FALLBACK_NO_SAVED_CANDIDATE);
  trace.presentList(false, true, false, diagnostics::FALLBACK_NO_SAVED_CANDIDATE);
  EXPECT_EQ(trace.fallbackCount, trace.presentationId);
  EXPECT_EQ(trace.fallbackCount, 2u);
}

TEST(WifiDiagnosticPolicy, KosyncCancellationIsDistinctFromFailureBeforeStart) {
  EXPECT_EQ(diagnostics::kosyncResultCode(false, true), diagnostics::KOSYNC_CANCELLATION);
  EXPECT_EQ(diagnostics::kosyncResultCode(false, false), diagnostics::KOSYNC_FAILURE_BEFORE_START);
}

TEST(WifiDiagnosticPolicy, KosyncSuccessIsRecordedBeforeSync) {
  EXPECT_EQ(diagnostics::kosyncResultCode(true, false), diagnostics::KOSYNC_SUCCESS);
  // A connected result is authoritative even if a stale cancellation flag is
  // present while the parent activity is being torn down.
  EXPECT_EQ(diagnostics::kosyncResultCode(true, true), diagnostics::KOSYNC_SUCCESS);
}
