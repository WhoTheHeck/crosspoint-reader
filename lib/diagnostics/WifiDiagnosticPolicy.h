#pragma once

#include <cstdint>

namespace diagnostics {

// These values are part of the diagnostic journal payload ABI. Keep the
// numeric values in sync with DiagnosticJournal::FallbackTrigger.
constexpr uint8_t FALLBACK_DRIVER_FAILURE = 0;
constexpr uint8_t FALLBACK_TIMEOUT = 1;
constexpr uint8_t FALLBACK_SCAN_FAILURE = 2;
constexpr uint8_t FALLBACK_NO_SAVED_CANDIDATE = 3;
constexpr uint8_t FALLBACK_USER_CONFIRM = 4;

constexpr uint16_t latestDisconnectReason(const bool known, const uint16_t reason) {
  return known ? reason : 0xffff;
}

// Fallback causes are intentionally ordered by evidence. A user-confirmed
// list is distinct from a driver/scan failure even when both happen together.
constexpr uint8_t selectFallbackTrigger(const bool explicitConfirm, const bool scanFailure,
                                         const bool hasCarriedCause, const uint8_t carriedCause) {
  if (explicitConfirm) return FALLBACK_USER_CONFIRM;
  if (scanFailure) return FALLBACK_SCAN_FAILURE;
  if (hasCarriedCause && (carriedCause == FALLBACK_DRIVER_FAILURE || carriedCause == FALLBACK_TIMEOUT)) {
    return carriedCause;
  }
  return FALLBACK_NO_SAVED_CANDIDATE;
}

// A failed saved-network attempt is intermediate while another saved
// candidate remains. It must not create a second list-presentation fallback.
constexpr bool isIntermediateAutoFailure(const bool hasUntriedSavedCandidate) {
  return hasUntriedSavedCandidate;
}

constexpr uint8_t KOSYNC_SUCCESS = 0;
constexpr uint8_t KOSYNC_CANCELLATION = 1;
constexpr uint8_t KOSYNC_FAILURE_BEFORE_START = 2;

constexpr uint8_t kosyncResultCode(const bool connected, const bool userCancelled) {
  if (connected) return KOSYNC_SUCCESS;
  return userCancelled ? KOSYNC_CANCELLATION : KOSYNC_FAILURE_BEFORE_START;
}

}  // namespace diagnostics
