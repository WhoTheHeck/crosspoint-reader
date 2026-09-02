#include "DiagnosticJournal.h"

#include <cstring>

#if defined(CROSSPOINT_DIAGNOSTICS_X3)
#include <Arduino.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include <ctime>
#endif

namespace {
static_assert(static_cast<uint8_t>(DiagnosticJournal::FallbackTrigger::DriverFailure) == 0);
static_assert(static_cast<uint8_t>(DiagnosticJournal::FallbackTrigger::Timeout) == 1);
static_assert(static_cast<uint8_t>(DiagnosticJournal::FallbackTrigger::ScanFailure) == 2);
static_assert(static_cast<uint8_t>(DiagnosticJournal::FallbackTrigger::NoSavedCandidate) == 3);
static_assert(static_cast<uint8_t>(DiagnosticJournal::FallbackTrigger::UserConfirm) == 4);
static_assert(static_cast<uint8_t>(DiagnosticJournal::KosyncResult::Success) == 0);
static_assert(static_cast<uint8_t>(DiagnosticJournal::KosyncResult::Cancellation) == 1);
static_assert(static_cast<uint8_t>(DiagnosticJournal::KosyncResult::FailureBeforeStart) == 2);
constexpr uint16_t BATTERY_KNOWN_SUPPORTED = 1u << 0;
constexpr uint16_t BATTERY_KNOWN_PERCENTAGE = 1u << 1;
constexpr uint16_t BATTERY_KNOWN_MILLIVOLTS = 1u << 2;
constexpr uint16_t BATTERY_KNOWN_CHARGING = 1u << 3;
constexpr uint16_t BATTERY_KNOWN_EXTERNAL_POWER = 1u << 4;
constexpr uint16_t BATTERY_VALUE_CHARGING = 1u << 0;
constexpr uint16_t BATTERY_VALUE_EXTERNAL_POWER = 1u << 1;

#if defined(CROSSPOINT_DIAGNOSTICS_X3)
uint64_t currentRtcSeconds() {
  const time_t now = time(nullptr);
  return now > 0 ? static_cast<uint64_t>(now) : 0;
}
#endif

void put16(uint8_t* destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* destination, const uint32_t value) {
  for (size_t i = 0; i < 4; ++i) destination[i] = static_cast<uint8_t>(value >> (i * 8));
}

void put64(uint8_t* destination, const uint64_t value) {
  for (size_t i = 0; i < 8; ++i) destination[i] = static_cast<uint8_t>(value >> (i * 8));
}

}  // namespace

bool DiagnosticJournal::begin(const uint32_t bootId) {
#if defined(CROSSPOINT_DIAGNOSTICS_X3)
  if (initialized_) return !failed_;
  bootId_ = bootId != 0 ? bootId : static_cast<uint32_t>(esp_random());
  if (!Storage.ensureDirectoryExists("/.crosspoint/diagnostics")) {
    LOG_ERR("DIAG", "Cannot create diagnostic directory");
    failed_ = true;
    return false;
  }

  file_ = Storage.open(PATH, O_RDWR | O_CREAT);
  if (!file_) {
    LOG_ERR("DIAG", "Cannot open diagnostic journal");
    failed_ = true;
    return false;
  }

  const uint64_t size = file_.fileSize64();
  const uint64_t alignedSize = size - (size % diagnostics::RECORD_SIZE);
  // HalStorage intentionally exposes no truncate primitive. Seeking to the
  // aligned boundary and writing the next complete record overwrites any
  // incomplete tail while retaining all complete records before it.
  if (!file_.seek64(alignedSize)) {
    LOG_ERR("DIAG", "Cannot seek diagnostic journal");
    file_.close();
    failed_ = true;
    return false;
  }
  initialized_ = true;
  failed_ = false;
  lastUptime32_ = millis();
  uptimeEpochMs_ = 0;
  return true;
#else
  (void)bootId;
  return false;
#endif
}

uint64_t DiagnosticJournal::extendedUptimeMs() {
#if defined(CROSSPOINT_DIAGNOSTICS_X3)
  const uint32_t now = millis();
  if (now < lastUptime32_) uptimeEpochMs_ += (uint64_t{1} << 32);
  lastUptime32_ = now;
  return uptimeEpochMs_ + now;
#else
  return 0;
#endif
}

bool DiagnosticJournal::append(const diagnostics::Event event, const uint8_t* payload, const size_t payloadLength,
                                const uint16_t flags, const bool flushNow, const uint64_t rtcSeconds) {
  return appendPayload(event, payload, payloadLength, flags, flushNow, rtcSeconds);
}

bool DiagnosticJournal::appendPayload(const diagnostics::Event event, const uint8_t* payload,
                                      const size_t payloadLength, const uint16_t flags, const bool flushNow,
                                      const uint64_t rtcSeconds) {
#if defined(CROSSPOINT_DIAGNOSTICS_X3)
  if (!initialized_ || failed_ || payloadLength > diagnostics::RECORD_PAYLOAD_SIZE ||
      (payloadLength > 0 && payload == nullptr)) {
    return false;
  }

  diagnostics::RecordFields fields;
  fields.sequence = sequence_++;
  fields.bootId = bootId_;
  fields.event = static_cast<uint16_t>(event);
  fields.payloadLength = static_cast<uint16_t>(payloadLength);
  fields.flags = flags;
  fields.uptimeMs = extendedUptimeMs();
  fields.rtcSeconds = rtcSeconds != 0 ? rtcSeconds : currentRtcSeconds();
  if (payload != nullptr && payloadLength > 0) std::memcpy(fields.payload.data(), payload, payloadLength);

  std::array<uint8_t, diagnostics::RECORD_SIZE> wire;
  if (!diagnostics::pack(fields, wire)) {
    failed_ = true;
    return false;
  }
  const size_t expectedSize = file_.position() + wire.size();
  if (file_.write(wire.data(), wire.size()) != wire.size()) {
    LOG_ERR("DIAG", "Diagnostic journal write failed");
    failed_ = true;
    return false;
  }
  if (flushNow) file_.flush();
  // HalFile::flush() has a void API; verify the post-write file position and
  // size so a failed media write is still visible and inhibits sleep.
  if (file_.position() != expectedSize || (flushNow && file_.fileSize64() != expectedSize)) {
    LOG_ERR("DIAG", "Diagnostic journal flush/size verification failed");
    failed_ = true;
    return false;
  }
  return true;
#else
  (void)event;
  (void)payload;
  (void)payloadLength;
  (void)flags;
  (void)flushNow;
  (void)rtcSeconds;
  return false;
#endif
}

void DiagnosticJournal::close() {
#if defined(CROSSPOINT_DIAGNOSTICS_X3)
  if (file_) {
    const uint64_t expectedSize = file_.position();
    file_.flush();
    if (file_.fileSize64() != expectedSize) failed_ = true;
    if (!file_.close()) failed_ = true;
  }
#endif
  initialized_ = false;
}

bool DiagnosticJournal::recordBoot(const uint32_t wakeCause, const uint64_t rtcSeconds) {
  uint8_t payload[4];
  put32(payload, wakeCause);
  return appendPayload(diagnostics::Event::Boot, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordBatterySample(const DiagnosticBatterySample& sample, const uint64_t rtcSeconds) {
  uint8_t payload[18] = {};
  uint16_t known = (sample.supported ? BATTERY_KNOWN_SUPPORTED : 0) |
                   (sample.percentageKnown ? BATTERY_KNOWN_PERCENTAGE : 0) |
                   (sample.millivoltsKnown ? BATTERY_KNOWN_MILLIVOLTS : 0) |
                   (sample.chargingKnown ? BATTERY_KNOWN_CHARGING : 0) |
                   (sample.externalPowerKnown ? BATTERY_KNOWN_EXTERNAL_POWER : 0);
  const uint16_t values = (sample.charging ? BATTERY_VALUE_CHARGING : 0) |
                          (sample.externalPower ? BATTERY_VALUE_EXTERNAL_POWER : 0);
  put16(payload, sample.percentage);
  put16(payload + 2, sample.millivolts);
  put16(payload + 4, known);
  put16(payload + 6, values);
  put32(payload + 8, static_cast<uint32_t>(sample.pm1VinMv));
  put32(payload + 12, static_cast<uint32_t>(sample.pm1VinOutMv));
  put16(payload + 16, static_cast<uint16_t>(sample.pm1PowerSource));
  return appendPayload(diagnostics::Event::BatterySample, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordSleepEnter(const uint8_t fromTimeout, const uint64_t rtcSeconds) {
  return appendPayload(diagnostics::Event::SleepEnter, &fromTimeout, sizeof(fromTimeout), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiAutoStart(const uint32_t attemptId, const uint32_t sessionId,
                                             const uint16_t credentialIndex, const bool isLastConnectedSsid,
                                             const uint8_t mode, const uint8_t origin, const uint64_t rtcSeconds) {
  uint8_t payload[16] = {};
  put32(payload, attemptId);
  put32(payload + 4, sessionId);
  put16(payload + 8, credentialIndex);
  payload[10] = isLastConnectedSsid ? 1 : 0;
  payload[11] = mode;
  payload[12] = origin;
  return appendPayload(diagnostics::Event::WifiAutoStart, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiDriverDisconnect(const uint32_t attemptId, const uint32_t sessionId,
                                                    const uint16_t reason, const uint16_t status,
                                                    const uint32_t elapsedMs, const uint64_t callbackUptimeMs,
                                                    const uint8_t mode, const uint8_t droppedCount,
                                                    const uint64_t rtcSeconds) {
  uint8_t payload[32] = {};
  put32(payload, attemptId);
  put32(payload + 4, sessionId);
  put16(payload + 8, reason);
  put16(payload + 10, status);
  put32(payload + 12, elapsedMs);
  put64(payload + 16, callbackUptimeMs);
  payload[24] = mode;
  payload[25] = droppedCount;
  return appendPayload(diagnostics::Event::WifiDriverDisconnect, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiAutoFailure(const uint32_t attemptId, const uint32_t sessionId,
                                               const uint16_t status, const uint32_t elapsedMs,
                                               const uint16_t latestReason, const uint8_t mode,
                                               const uint64_t rtcSeconds) {
  uint8_t payload[16] = {};
  put32(payload, attemptId);
  put32(payload + 4, sessionId);
  put16(payload + 8, status);
  put16(payload + 10, latestReason);
  put32(payload + 12, elapsedMs);
  payload[14] = mode;
  return appendPayload(diagnostics::Event::WifiAutoFailure, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiAutoTimeout(const uint32_t attemptId, const uint32_t sessionId,
                                               const uint16_t status, const uint32_t elapsedMs,
                                               const uint16_t latestReason, const uint8_t mode,
                                               const uint64_t rtcSeconds) {
  uint8_t payload[16] = {};
  put32(payload, attemptId);
  put32(payload + 4, sessionId);
  put16(payload + 8, status);
  put16(payload + 10, latestReason);
  put32(payload + 12, elapsedMs);
  payload[14] = mode;
  return appendPayload(diagnostics::Event::WifiAutoTimeout, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiScanFailed(const uint32_t sessionId, const int32_t result,
                                              const uint64_t rtcSeconds) {
  uint8_t payload[8] = {};
  put32(payload, sessionId);
  put32(payload + 4, static_cast<uint32_t>(result));
  return appendPayload(diagnostics::Event::WifiScanFailed, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiListFallback(const uint32_t sessionId, const uint32_t presentationId,
                                               const FallbackTrigger trigger, const uint64_t rtcSeconds) {
  uint8_t payload[12] = {};
  put32(payload, sessionId);
  put32(payload + 4, presentationId);
  payload[8] = static_cast<uint8_t>(trigger);
  return appendPayload(diagnostics::Event::WifiListFallback, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordWifiConnected(const uint32_t attemptId, const uint32_t sessionId,
                                            const uint32_t elapsedMs, const int16_t rssi, const uint8_t channel,
                                            const bool savedCredential, const uint16_t credentialIndex,
                                            const uint8_t mode, const uint8_t origin, const uint64_t rtcSeconds) {
  uint8_t payload[24] = {};
  put32(payload, attemptId);
  put32(payload + 4, sessionId);
  put32(payload + 8, elapsedMs);
  put16(payload + 12, static_cast<uint16_t>(rssi));
  payload[14] = channel;
  payload[15] = savedCredential ? 1 : 0;
  put16(payload + 16, credentialIndex);
  payload[18] = mode;
  payload[19] = origin;
  return appendPayload(diagnostics::Event::WifiConnected, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordKosyncWifiResult(const uint32_t sessionId, const KosyncResult result,
                                               const uint64_t rtcSeconds) {
  uint8_t payload[8] = {};
  put32(payload, sessionId);
  payload[4] = static_cast<uint8_t>(result);
  return appendPayload(diagnostics::Event::KosyncWifiResult, payload, sizeof(payload), 0, true, rtcSeconds);
}

bool DiagnosticJournal::recordQueueOverflow(const uint32_t sessionId, const uint32_t droppedCount,
                                            const uint64_t rtcSeconds) {
  uint8_t payload[8] = {};
  put32(payload, sessionId);
  put32(payload + 4, droppedCount);
  return appendPayload(diagnostics::Event::QueueOverflow, payload, sizeof(payload), 0, true, rtcSeconds);
}
