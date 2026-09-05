#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <utility>

#include "KOReaderSyncClient.h"

// Lifecycle events are deliberately typed so callers cannot accidentally use
// an automatic sync as a manual screen flow.
enum class KOReaderSyncTrigger : uint8_t {
  Manual,
  Open,
  Wake,
  Close,
  Home,
  BookSwitch,
  Sleep,
};

enum class KOReaderSyncMode : uint8_t { Ask, Smart };

enum class KOReaderSyncComparison : uint8_t { Equal, LocalAhead, RemoteAhead, Missing, Unknown };

enum class KOReaderSyncPendingTransition : uint8_t {
  None,
  ApplyRemote,
  UploadLocal,
  Prompt,
  ContinueSleep,
};

struct KOReaderSyncSnapshot {
  std::string documentHash;
  KOReaderProgress localProgress;
};

struct KOReaderSyncOutcome {
  KOReaderSyncClient::Error error = KOReaderSyncClient::OK;
  KOReaderSyncComparison comparison = KOReaderSyncComparison::Unknown;
  KOReaderSyncPendingTransition pending = KOReaderSyncPendingTransition::None;
  KOReaderProgress remoteProgress;
  bool hasRemoteProgress = false;
};

/**
 * Non-visual orchestration for one automatic KOSync exchange.
 *
 * A session fetches the configured record and performs a PUT only after its
 * presenter supplies a resolved comparison. Both requests use the remaining
 * portion of one absolute 15-second deadline. The activity owns rendering and
 * maps the remote anchor while the book is available.
 */
class KOReaderSyncSession final {
 public:
  static constexpr uint32_t DEADLINE_MS = 15000;

  KOReaderSyncSession(KOReaderSyncSnapshot snapshot, KOReaderSyncTrigger trigger, KOReaderSyncMode mode,
                      uint32_t startedAtMs, KOReaderSyncTransport& transport);

  KOReaderSyncOutcome run(uint32_t nowMs);
  KOReaderSyncOutcome resolve(KOReaderSyncComparison comparison, uint32_t nowMs);

  static bool deadlineExpired(uint32_t nowMs, uint32_t startedAtMs);
  static uint32_t remainingMs(uint32_t nowMs, uint32_t startedAtMs);
  static constexpr bool shouldDisconnectAutomaticWifi(const bool wasAlreadyConnected, const bool syncActivatedWifi) {
    return syncActivatedWifi && !wasAlreadyConnected;
  }
  static constexpr bool suppressImmediateOpen(const KOReaderSyncTrigger trigger) {
    return trigger != KOReaderSyncTrigger::Sleep;
  }
  static KOReaderSyncPendingTransition decide(const KOReaderSyncTrigger trigger, const KOReaderSyncMode mode,
                                               const KOReaderSyncComparison comparison) {
    if (trigger == KOReaderSyncTrigger::Sleep) {
      return (comparison == KOReaderSyncComparison::LocalAhead || comparison == KOReaderSyncComparison::Missing)
                 ? KOReaderSyncPendingTransition::UploadLocal
                 : KOReaderSyncPendingTransition::ContinueSleep;
    }
    if (comparison == KOReaderSyncComparison::Equal) return KOReaderSyncPendingTransition::None;
    if (mode == KOReaderSyncMode::Ask || comparison == KOReaderSyncComparison::Unknown) {
      return KOReaderSyncPendingTransition::Prompt;
    }
    switch (comparison) {
      case KOReaderSyncComparison::LocalAhead:
      case KOReaderSyncComparison::Missing:
        return KOReaderSyncPendingTransition::UploadLocal;
      case KOReaderSyncComparison::RemoteAhead:
        return KOReaderSyncPendingTransition::ApplyRemote;
      case KOReaderSyncComparison::Equal:
      case KOReaderSyncComparison::Unknown:
        return KOReaderSyncPendingTransition::Prompt;
    }
    return KOReaderSyncPendingTransition::Prompt;
  }

 private:
  KOReaderSyncSnapshot snapshot;
  KOReaderSyncTrigger trigger;
  KOReaderSyncMode mode;
  uint32_t startedAtMs;
  KOReaderSyncTransport& transport;

  static bool validPercentage(float percentage);
  bool validSnapshot() const;
  static bool sleepTrigger(KOReaderSyncTrigger trigger) { return trigger == KOReaderSyncTrigger::Sleep; }
};
