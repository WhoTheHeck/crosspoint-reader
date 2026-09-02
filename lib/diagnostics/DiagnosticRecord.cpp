#include "DiagnosticRecord.h"

#include <cstring>

namespace diagnostics {
namespace {
constexpr uint8_t MAGIC[4] = {'X', '3', 'D', 'G'};

void put16(uint8_t* destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* destination, const uint32_t value) {
  for (size_t i = 0; i < 4; ++i) destination[i] = static_cast<uint8_t>(value >> (8 * i));
}

void put64(uint8_t* destination, const uint64_t value) {
  for (size_t i = 0; i < 8; ++i) destination[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t get16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1] << 8);
}

uint32_t get32(const uint8_t* source) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(source[i]) << (8 * i);
  return value;
}

uint64_t get64(const uint8_t* source) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(source[i]) << (8 * i);
  return value;
}
}  // namespace

uint32_t crc32(const uint8_t* bytes, const size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(crc & 1u)));
    }
  }
  return ~crc;
}

bool pack(const RecordFields& fields, std::array<uint8_t, RECORD_SIZE>& output) {
  if (fields.payloadLength > RECORD_PAYLOAD_SIZE) return false;
  output.fill(0);
  std::memcpy(output.data(), MAGIC, sizeof(MAGIC));
  put16(output.data() + 4, RECORD_VERSION);
  put16(output.data() + 6, fields.event);
  put16(output.data() + 8, fields.payloadLength);
  put16(output.data() + 10, fields.flags);
  put32(output.data() + 12, fields.sequence);
  put32(output.data() + 16, fields.bootId);
  put64(output.data() + 20, fields.uptimeMs);
  put64(output.data() + 28, fields.rtcSeconds);
  if (fields.payloadLength > 0) {
    std::memcpy(output.data() + RECORD_PAYLOAD_OFFSET, fields.payload.data(), fields.payloadLength);
  }
  put32(output.data() + RECORD_CRC_OFFSET, crc32(output.data(), RECORD_CRC_OFFSET));
  return true;
}

bool unpackBytes(const uint8_t* input, RecordFields& output) {
  if (std::memcmp(input, MAGIC, sizeof(MAGIC)) != 0 || get16(input + 4) != RECORD_VERSION) return false;
  const uint16_t payloadLength = get16(input + 8);
  if (payloadLength > RECORD_PAYLOAD_SIZE || get32(input + RECORD_CRC_OFFSET) != crc32(input, RECORD_CRC_OFFSET)) {
    return false;
  }
  output = {};
  output.sequence = get32(input + 12);
  output.bootId = get32(input + 16);
  output.event = get16(input + 6);
  output.payloadLength = payloadLength;
  output.flags = get16(input + 10);
  output.uptimeMs = get64(input + 20);
  output.rtcSeconds = get64(input + 28);
  if (payloadLength > 0) std::memcpy(output.payload.data(), input + RECORD_PAYLOAD_OFFSET, payloadLength);
  return true;
}

bool unpack(const uint8_t (&input)[RECORD_SIZE], RecordFields& output) { return unpackBytes(input, output); }

bool unpack(const std::array<uint8_t, RECORD_SIZE>& input, RecordFields& output) {
  return unpackBytes(input.data(), output);
}

bool isKnownEvent(const uint16_t event) {
  switch (static_cast<Event>(event)) {
    case Event::Boot:
    case Event::BatterySample:
    case Event::SleepEnter:
    case Event::WifiAutoStart:
    case Event::WifiDriverDisconnect:
    case Event::WifiAutoFailure:
    case Event::WifiAutoTimeout:
    case Event::WifiScanFailed:
    case Event::WifiListFallback:
    case Event::WifiConnected:
    case Event::KosyncWifiResult:
    case Event::QueueOverflow:
      return true;
  }
  return false;
}

const char* eventName(const uint16_t event) {
  switch (static_cast<Event>(event)) {
    case Event::Boot:
      return "boot";
    case Event::BatterySample:
      return "battery_sample";
    case Event::SleepEnter:
      return "sleep_enter";
    case Event::WifiAutoStart:
      return "wifi_auto_start";
    case Event::WifiDriverDisconnect:
      return "wifi_driver_disconnect";
    case Event::WifiAutoFailure:
      return "wifi_auto_failure";
    case Event::WifiAutoTimeout:
      return "wifi_auto_timeout";
    case Event::WifiScanFailed:
      return "wifi_scan_failed";
    case Event::WifiListFallback:
      return "wifi_list_fallback";
    case Event::WifiConnected:
      return "wifi_connected";
    case Event::KosyncWifiResult:
      return "kosync_wifi_result";
    case Event::QueueOverflow:
      return "queue_overflow";
  }
  return "unknown";
}

}  // namespace diagnostics
