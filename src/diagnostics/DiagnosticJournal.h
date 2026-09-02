#pragma once

#include <DiagnosticRecord.h>

#include <cstddef>
#include <cstdint>

#if defined(CROSSPOINT_DIAGNOSTICS_X3)
#include <HalStorage.h>
#endif

struct DiagnosticBatterySample {
  bool supported = false;
  bool percentageKnown = false;
  bool millivoltsKnown = false;
  bool chargingKnown = false;
  bool externalPowerKnown = false;
  uint16_t percentage = 0;
  uint16_t millivolts = 0;
  bool charging = false;
  bool externalPower = false;
  int32_t pm1VinMv = -1;
  int32_t pm1VinOutMv = -1;
  int16_t pm1PowerSource = -1;
};

class DiagnosticJournal {
 public:
  static constexpr char PATH[] = "/.crosspoint/diagnostics/x3-diagnostic.bin";
  static constexpr uint16_t UNKNOWN_CREDENTIAL_INDEX = 0xffff;
  static constexpr uint32_t UNKNOWN_REASON = 0xffff;

  enum class FallbackTrigger : uint8_t {
    DriverFailure = 0,
    Timeout = 1,
    ScanFailure = 2,
    NoSavedCandidate = 3,
    UserConfirm = 4,
  };

  enum class KosyncResult : uint8_t {
    Success = 0,
    Cancellation = 1,
    FailureBeforeStart = 2,
  };

  bool begin(uint32_t bootId = 0);
  bool append(diagnostics::Event event, const uint8_t* payload = nullptr, size_t payloadLength = 0,
              uint16_t flags = 0, bool flushNow = false, uint64_t rtcSeconds = 0);
  void close();

  [[nodiscard]] bool healthy() const { return initialized_ && !failed_; }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] uint32_t sequence() const { return sequence_; }

  bool recordBoot(uint32_t wakeCause, uint64_t rtcSeconds = 0);
  bool recordBatterySample(const DiagnosticBatterySample& sample, uint64_t rtcSeconds = 0);
  bool recordSleepEnter(uint8_t fromTimeout, uint64_t rtcSeconds = 0);
  bool recordWifiAutoStart(uint32_t attemptId, uint32_t sessionId, uint16_t credentialIndex,
                           bool isLastConnectedSsid, uint8_t mode, uint8_t origin, uint64_t rtcSeconds = 0);
  bool recordWifiDriverDisconnect(uint32_t attemptId, uint32_t sessionId, uint16_t reason, uint16_t status,
                                  uint32_t elapsedMs, uint64_t callbackUptimeMs, uint8_t mode, uint8_t droppedCount,
                                  uint64_t rtcSeconds = 0);
  bool recordWifiAutoFailure(uint32_t attemptId, uint32_t sessionId, uint16_t status, uint32_t elapsedMs,
                             uint16_t latestReason, uint8_t mode, uint64_t rtcSeconds = 0);
  bool recordWifiAutoTimeout(uint32_t attemptId, uint32_t sessionId, uint16_t status, uint32_t elapsedMs,
                             uint16_t latestReason, uint8_t mode, uint64_t rtcSeconds = 0);
  bool recordWifiScanFailed(uint32_t sessionId, int32_t result, uint64_t rtcSeconds = 0);
  bool recordWifiListFallback(uint32_t sessionId, uint32_t presentationId, FallbackTrigger trigger,
                              uint64_t rtcSeconds = 0);
  bool recordWifiConnected(uint32_t attemptId, uint32_t sessionId, uint32_t elapsedMs, int16_t rssi, uint8_t channel,
                           bool savedCredential, uint16_t credentialIndex, uint8_t mode, uint8_t origin,
                           uint64_t rtcSeconds = 0);
  bool recordKosyncWifiResult(uint32_t sessionId, KosyncResult result, uint64_t rtcSeconds = 0);
  bool recordQueueOverflow(uint32_t sessionId, uint32_t droppedCount, uint64_t rtcSeconds = 0);

 private:
  bool appendPayload(diagnostics::Event event, const uint8_t* payload, size_t payloadLength, uint16_t flags,
                     bool flushNow, uint64_t rtcSeconds);
  uint64_t extendedUptimeMs();

#if defined(CROSSPOINT_DIAGNOSTICS_X3)
  HalFile file_;
#endif
  bool initialized_ = false;
  bool failed_ = false;
  uint32_t sequence_ = 0;
#if defined(CROSSPOINT_DIAGNOSTICS_X3)
  uint32_t bootId_ = 0;
  uint32_t lastUptime32_ = 0;
  uint64_t uptimeEpochMs_ = 0;
#endif
};

#if defined(CROSSPOINT_DIAGNOSTICS_X3)
extern DiagnosticJournal diagnosticJournal;
#endif
