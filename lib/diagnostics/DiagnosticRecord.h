#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace diagnostics {

constexpr size_t RECORD_SIZE = 96;
constexpr size_t RECORD_PAYLOAD_OFFSET = 36;
constexpr size_t RECORD_PAYLOAD_SIZE = 56;
constexpr size_t RECORD_CRC_OFFSET = 92;
constexpr uint16_t RECORD_VERSION = 1;
static_assert(RECORD_PAYLOAD_OFFSET + RECORD_PAYLOAD_SIZE == RECORD_CRC_OFFSET);
static_assert(RECORD_CRC_OFFSET + sizeof(uint32_t) == RECORD_SIZE);

enum class Event : uint16_t {
  Boot = 1,
  BatterySample = 2,
  SleepEnter = 3,
  WifiAutoStart = 16,
  WifiDriverDisconnect = 17,
  WifiAutoFailure = 18,
  WifiAutoTimeout = 19,
  WifiScanFailed = 20,
  WifiListFallback = 21,
  WifiConnected = 22,
  KosyncWifiResult = 23,
  QueueOverflow = 24,
};

struct RecordFields {
  uint32_t sequence = 0;
  uint32_t bootId = 0;
  uint16_t event = 0;
  uint16_t payloadLength = 0;
  uint64_t uptimeMs = 0;
  uint64_t rtcSeconds = 0;
  uint16_t flags = 0;
  std::array<uint8_t, RECORD_PAYLOAD_SIZE> payload{};
};

uint32_t crc32(const uint8_t* bytes, size_t length);

// Packs all multi-byte fields explicitly little-endian. The output is always
// exactly RECORD_SIZE bytes and contains no padding or host ABI dependencies.
bool pack(const RecordFields& fields, std::array<uint8_t, RECORD_SIZE>& output);

// Validates magic, version, payload bounds, and CRC before decoding fields.
bool unpack(const uint8_t (&input)[RECORD_SIZE], RecordFields& output);
bool unpack(const std::array<uint8_t, RECORD_SIZE>& input, RecordFields& output);

const char* eventName(uint16_t event);
bool isKnownEvent(uint16_t event);

}  // namespace diagnostics
