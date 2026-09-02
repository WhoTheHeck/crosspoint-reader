#!/usr/bin/env python3
"""Deterministic compatibility and leakage tests for the journal decoder."""

from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import decode_diagnostic_journal as decoder  # noqa: E402


def make_record(sequence: int, boot_id: int, event_id: int, payload: bytes = b"") -> bytes:
    raw = bytearray(decoder.RECORD_SIZE)
    struct.pack_into("<4sHHHHIIQQ", raw, 0, decoder.MAGIC, 1, event_id, len(payload), 0,
                     sequence, boot_id, sequence * 1000, 1_700_000_000 + sequence)
    raw[decoder.PAYLOAD_OFFSET : decoder.PAYLOAD_OFFSET + len(payload)] = payload
    struct.pack_into("<I", raw, decoder.CRC_OFFSET, decoder.crc32(raw[: decoder.CRC_OFFSET]))
    return bytes(raw)


class DiagnosticDecoderTest(unittest.TestCase):
    def decode_bytes(self, data: bytes) -> dict:
        with tempfile.NamedTemporaryFile() as journal:
            journal.write(data)
            journal.flush()
            return decoder.decode(Path(journal.name))

    def test_old_battery_and_extended_records_decode_with_numeric_fields(self) -> None:
        battery_payload = struct.pack("<HHHHiih", 83, 4012, 0x1F, 0x01, 5000, 4900, 1)
        payloads = {
            1: struct.pack("<I", 2),
            2: battery_payload,
            3: b"\x01",
            16: struct.pack("<IIHBBB", 4, 9, 2, 1, 1, 1),
            17: struct.pack("<IIHHIQBB", 4, 9, 201, 3, 120, 1234, 1, 0),
            18: struct.pack("<IIHHIB", 4, 9, 4, 201, 121, 1),
            19: struct.pack("<IIHHIB", 4, 9, 3, 201, 7001, 1),
            20: struct.pack("<Ii", 9, -2),
            21: struct.pack("<IIB", 9, 3, 1),
            22: struct.pack("<IIihBBHBB", 4, 9, 700, -53, 6, 1, 2, 1, 1),
            23: struct.pack("<IB", 9, 0),
            24: struct.pack("<II", 9, 7),
        }
        result = self.decode_bytes(b"".join(make_record(index, 77, event, payload)
                                           for index, (event, payload) in enumerate(payloads.items())))
        self.assertEqual(result["record_count"], len(payloads))
        self.assertEqual(result["diagnostics"]["crc_errors"], 0)
        self.assertEqual(result["diagnostics"]["sequence_gaps"], [])
        self.assertEqual(result["records"][1]["fields"]["percentage"], 83)
        self.assertEqual(result["records"][8]["fields"]["trigger_name"], "timeout")
        self.assertEqual(result["records"][9]["fields"]["saved_credential"], True)
        self.assertEqual(result["records"][10]["fields"]["result_name"], "success")
        expected_fields = {
            1: {"wake_cause"},
            2: {"percentage", "millivolts", "known_flags", "value_flags", "pm1_vin_mv",
                "pm1_vin_out_mv", "pm1_power_source"},
            3: {"from_timeout"},
            16: {"attempt_id", "session_id", "credential_index", "is_last_connected_ssid", "mode", "origin"},
            17: {"attempt_id", "session_id", "reason", "status", "elapsed_ms", "callback_uptime_ms", "mode",
                "dropped_count"},
            18: {"attempt_id", "session_id", "status", "latest_reason", "elapsed_ms", "mode"},
            19: {"attempt_id", "session_id", "status", "latest_reason", "elapsed_ms", "mode"},
            20: {"session_id", "scan_result"},
            21: {"session_id", "presentation_id", "trigger", "trigger_name"},
            22: {"attempt_id", "session_id", "elapsed_ms", "rssi", "channel", "saved_credential",
                "credential_index", "mode", "origin"},
            23: {"session_id", "result", "result_name"},
            24: {"session_id", "dropped_count"},
        }
        for index, event_id in enumerate(payloads):
            self.assertEqual(set(result["records"][index]["fields"]), expected_fields[event_id])

    def test_tail_crc_gap_unknown_and_secret_redaction(self) -> None:
        secret_values = b"KnownSSID seeded-password https://sync.example token-123 192.0.2.4 aa:bb:cc:dd:ee:ff"
        first = make_record(0, 88, 1, struct.pack("<I", 1))
        unknown_enum = make_record(1, 88, 21, struct.pack("<IIB", 9, 3, 9))
        second = bytearray(make_record(3, 88, 0x9000, secret_values[: decoder.PAYLOAD_SIZE]))
        second[decoder.PAYLOAD_OFFSET] ^= 0x01
        result = self.decode_bytes(first + unknown_enum + bytes(second) + b"partial-tail")
        self.assertEqual(result["record_count"], 3)
        self.assertEqual(result["diagnostics"]["truncated_tail_bytes"], len(b"partial-tail"))
        self.assertEqual(result["diagnostics"]["crc_errors"], 1)
        self.assertEqual(result["diagnostics"]["unknown_events"], 1)
        self.assertEqual(result["diagnostics"]["unknown_enum_values"],
                         [{"index": 1, "event": "wifi_list_fallback", "field": "trigger", "value": 9}])
        self.assertEqual(result["diagnostics"]["sequence_gaps"], [{"boot_id": 88, "expected": 2, "actual": 3}])
        encoded = json.dumps(result, sort_keys=True)
        for secret in (b"KnownSSID", b"seeded-password", b"sync.example", b"token-123", b"192.0.2.4",
                       b"aa:bb:cc:dd:ee:ff"):
            self.assertNotIn(secret.decode("ascii"), encoded)


if __name__ == "__main__":
    unittest.main()
