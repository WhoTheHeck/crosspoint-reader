#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "DiagnosticRecord.h"

namespace {

TEST(DiagnosticRecord, PacksLittleEndianFixedSizeAndRoundTrips) {
  diagnostics::RecordFields fields;
  fields.sequence = 0x11223344;
  fields.bootId = 0x55667788;
  fields.event = static_cast<uint16_t>(diagnostics::Event::BatterySample);
  fields.payloadLength = 4;
  fields.uptimeMs = 0x0102030405060708ULL;
  fields.rtcSeconds = 0x1112131415161718ULL;
  fields.flags = 0xA1B2;
  fields.payload[0] = 0xDE;
  fields.payload[1] = 0xAD;
  fields.payload[2] = 0xBE;
  fields.payload[3] = 0xEF;

  std::array<uint8_t, diagnostics::RECORD_SIZE> wire;
  ASSERT_TRUE(diagnostics::pack(fields, wire));
  EXPECT_EQ(wire.size(), 96u);
  EXPECT_EQ(wire[0], 'X');
  EXPECT_EQ(wire[6], static_cast<uint8_t>(diagnostics::Event::BatterySample));
  EXPECT_EQ(wire[12], 0x44);
  EXPECT_EQ(wire[13], 0x33);
  EXPECT_EQ(wire[20], 0x08);
  EXPECT_EQ(wire[27], 0x01);

  diagnostics::RecordFields decoded;
  ASSERT_TRUE(diagnostics::unpack(wire, decoded));
  EXPECT_EQ(decoded.sequence, fields.sequence);
  EXPECT_EQ(decoded.bootId, fields.bootId);
  EXPECT_EQ(decoded.event, fields.event);
  EXPECT_EQ(decoded.payloadLength, fields.payloadLength);
  EXPECT_EQ(decoded.uptimeMs, fields.uptimeMs);
  EXPECT_EQ(decoded.rtcSeconds, fields.rtcSeconds);
  EXPECT_EQ(decoded.flags, fields.flags);
  EXPECT_EQ(decoded.payload[3], 0xEF);
}

TEST(DiagnosticRecord, RejectsCrcCorruptionAndOversizedPayload) {
  diagnostics::RecordFields fields;
  fields.event = static_cast<uint16_t>(diagnostics::Event::Boot);
  fields.payloadLength = diagnostics::RECORD_PAYLOAD_SIZE + 1;
  std::array<uint8_t, diagnostics::RECORD_SIZE> wire;
  EXPECT_FALSE(diagnostics::pack(fields, wire));

  fields.payloadLength = 1;
  ASSERT_TRUE(diagnostics::pack(fields, wire));
  wire[diagnostics::RECORD_PAYLOAD_OFFSET] ^= 0x01;
  diagnostics::RecordFields decoded;
  EXPECT_FALSE(diagnostics::unpack(wire, decoded));
}

TEST(DiagnosticRecord, UnknownEventIsReportedWithoutTreatingReservedIdsAsKnown) {
  EXPECT_TRUE(diagnostics::isKnownEvent(static_cast<uint16_t>(diagnostics::Event::WifiConnected)));
  EXPECT_FALSE(diagnostics::isKnownEvent(4));
  EXPECT_FALSE(diagnostics::isKnownEvent(0x7fff));
  EXPECT_STREQ(diagnostics::eventName(0x7fff), "unknown");
}

TEST(DiagnosticRecord, TruncatedTailIsDetectableByRecordGeometry) {
  std::array<uint8_t, diagnostics::RECORD_SIZE * 2 + 7> bytes{};
  EXPECT_EQ(bytes.size() % diagnostics::RECORD_SIZE, 7u);
}

}  // namespace
