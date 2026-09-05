#include "KOReaderSyncSession.h"

#include <cmath>
#include <utility>

KOReaderSyncSession::KOReaderSyncSession(KOReaderSyncSnapshot snapshot, const KOReaderSyncTrigger trigger,
                                         const KOReaderSyncMode mode, const uint32_t startedAtMs,
                                         KOReaderSyncTransport& transport)
    : snapshot(std::move(snapshot)), trigger(trigger), mode(mode), startedAtMs(startedAtMs), transport(transport) {}

bool KOReaderSyncSession::validPercentage(const float percentage) {
  return std::isfinite(percentage) && percentage >= 0.0f && percentage <= 1.0f;
}

bool KOReaderSyncSession::validSnapshot() const {
  return !snapshot.documentHash.empty() && !snapshot.localProgress.document.empty() &&
         !snapshot.localProgress.progress.empty() && validPercentage(snapshot.localProgress.percentage);
}

bool KOReaderSyncSession::deadlineExpired(const uint32_t nowMs, const uint32_t startedAtMs) {
  return static_cast<uint32_t>(nowMs - startedAtMs) >= DEADLINE_MS;
}

uint32_t KOReaderSyncSession::remainingMs(const uint32_t nowMs, const uint32_t startedAtMs) {
  const uint32_t elapsed = static_cast<uint32_t>(nowMs - startedAtMs);
  return elapsed >= DEADLINE_MS ? 0 : DEADLINE_MS - elapsed;
}

KOReaderSyncOutcome KOReaderSyncSession::run(const uint32_t nowMs) {
  KOReaderSyncOutcome outcome;
  if (!validSnapshot()) {
    outcome.error = KOReaderSyncClient::JSON_ERROR;
    outcome.pending = sleepTrigger(trigger) ? KOReaderSyncPendingTransition::ContinueSleep
                                             : KOReaderSyncPendingTransition::None;
    return outcome;
  }

  uint32_t remaining = remainingMs(nowMs, startedAtMs);
  if (remaining == 0) {
    outcome.error = KOReaderSyncClient::NETWORK_ERROR;
    outcome.pending = sleepTrigger(trigger) ? KOReaderSyncPendingTransition::ContinueSleep
                                             : KOReaderSyncPendingTransition::None;
    return outcome;
  }

  KOReaderProgress remote;
  const auto getResult = transport.getProgress(snapshot.documentHash, remote, remaining);
  if (getResult == KOReaderSyncClient::NOT_FOUND) {
    return resolve(KOReaderSyncComparison::Missing, transport.nowMs());
  }

  if (getResult != KOReaderSyncClient::OK) {
    outcome.error = getResult;
    outcome.pending = sleepTrigger(trigger) ? KOReaderSyncPendingTransition::ContinueSleep
                                             : KOReaderSyncPendingTransition::None;
    return outcome;
  }

  outcome.error = KOReaderSyncClient::OK;
  outcome.hasRemoteProgress = true;
  outcome.remoteProgress = std::move(remote);
  return outcome;
}

KOReaderSyncOutcome KOReaderSyncSession::resolve(const KOReaderSyncComparison comparison, const uint32_t nowMs) {
  KOReaderSyncOutcome outcome;
  outcome.comparison = comparison;
  outcome.pending = decide(trigger, mode, comparison);

  if (outcome.pending != KOReaderSyncPendingTransition::UploadLocal) {
    return outcome;
  }

  const uint32_t remaining = remainingMs(nowMs, startedAtMs);
  if (remaining == 0) {
    outcome.error = KOReaderSyncClient::NETWORK_ERROR;
    outcome.pending = sleepTrigger(trigger) ? KOReaderSyncPendingTransition::ContinueSleep
                                             : KOReaderSyncPendingTransition::None;
    return outcome;
  }

  outcome.error = transport.updateProgress(snapshot.localProgress, remaining);
  if (sleepTrigger(trigger)) {
    outcome.pending = KOReaderSyncPendingTransition::ContinueSleep;
  } else if (outcome.error == KOReaderSyncClient::OK) {
    // A successful Smart upload is terminal. The GET result that selected it
    // is no longer an actionable comparison.
    outcome.pending = KOReaderSyncPendingTransition::None;
  }
  return outcome;
}
