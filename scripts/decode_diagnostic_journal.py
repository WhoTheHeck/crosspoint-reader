#!/usr/bin/env python3
"""Decode the fixed-width X3 diagnostic journal without exposing identifiers."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
from pathlib import Path

RECORD_SIZE = 96
CRC_OFFSET = 92
PAYLOAD_OFFSET = 36
PAYLOAD_SIZE = 56
MAGIC = b"X3DG"
EVENTS = {
    1: "boot",
    2: "battery_sample",
    3: "sleep_enter",
    16: "wifi_auto_start",
    17: "wifi_driver_disconnect",
    18: "wifi_auto_failure",
    19: "wifi_auto_timeout",
    20: "wifi_scan_failed",
    21: "wifi_list_fallback",
    22: "wifi_connected",
    23: "kosync_wifi_result",
    24: "queue_overflow",
}
FALLBACK_TRIGGERS = {
    0: "driver_failure",
    1: "timeout",
    2: "scan_failure",
    3: "no_saved_candidate",
    4: "user_confirm",
}
KOSYNC_RESULTS = {0: "success", 1: "cancellation", 2: "failure_before_start"}
ATTEMPT_MODES = {0: "manual", 1: "automatic"}
SELECTION_ORIGINS = {0: "general", 1: "kosync"}


def crc32(data: bytes) -> int:
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xEDB88320 if value & 1 else 0)
    return (~value) & 0xFFFFFFFF


def payload_fields(event_id: int, payload: bytes) -> dict:
    def u16(offset: int) -> int:
        return struct.unpack_from("<H", payload, offset)[0]

    def i16(offset: int) -> int:
        return struct.unpack_from("<h", payload, offset)[0]

    def u32(offset: int) -> int:
        return struct.unpack_from("<I", payload, offset)[0]

    def i32(offset: int) -> int:
        return struct.unpack_from("<i", payload, offset)[0]

    def u64(offset: int) -> int:
        return struct.unpack_from("<Q", payload, offset)[0]

    fields = {}
    if event_id == 1 and len(payload) >= 4:
        fields["wake_cause"] = u32(0)
    elif event_id == 2 and len(payload) >= 18:
        fields.update({"percentage": u16(0), "millivolts": u16(2), "known_flags": u16(4),
                       "value_flags": u16(6), "pm1_vin_mv": i32(8), "pm1_vin_out_mv": i32(12),
                       "pm1_power_source": i16(16)})
    elif event_id == 3 and payload:
        fields["from_timeout"] = payload[0]
    elif event_id == 16 and len(payload) >= 13:
        fields.update({"attempt_id": u32(0), "session_id": u32(4), "credential_index": u16(8),
                       "is_last_connected_ssid": bool(payload[10]), "mode": payload[11], "origin": payload[12]})
    elif event_id in (18, 19) and len(payload) >= 15:
        fields.update({"attempt_id": u32(0), "session_id": u32(4), "status": u16(8),
                       "latest_reason": u16(10), "elapsed_ms": u32(12), "mode": payload[14]})
    elif event_id == 17 and len(payload) >= 26:
        fields.update({"attempt_id": u32(0), "session_id": u32(4), "reason": u16(8), "status": u16(10),
                       "elapsed_ms": u32(12), "callback_uptime_ms": u64(16), "mode": payload[24],
                       "dropped_count": payload[25]})
    elif event_id == 20 and len(payload) >= 8:
        fields.update({"session_id": u32(0), "scan_result": i32(4)})
    elif event_id == 21 and len(payload) >= 9:
        trigger = payload[8]
        fields.update({"session_id": u32(0), "presentation_id": u32(4), "trigger": trigger,
                       "trigger_name": FALLBACK_TRIGGERS.get(trigger, "unknown")})
    elif event_id == 22 and len(payload) >= 20:
        fields.update({"attempt_id": u32(0), "session_id": u32(4), "elapsed_ms": u32(8), "rssi": i16(12),
                       "channel": payload[14], "saved_credential": bool(payload[15]),
                       "credential_index": u16(16), "mode": payload[18], "origin": payload[19]})
    elif event_id == 23 and len(payload) >= 5:
        result = payload[4]
        fields.update({"session_id": u32(0), "result": result,
                       "result_name": KOSYNC_RESULTS.get(result, "unknown")})
    elif event_id == 24 and len(payload) >= 8:
        fields.update({"session_id": u32(0), "dropped_count": u32(4)})
    return fields


def decode_record(raw: bytes, index: int) -> dict:
    version, event_id, payload_length, flags = struct.unpack_from("<HHHH", raw, 4)
    sequence, boot_id = struct.unpack_from("<II", raw, 12)
    uptime_ms, rtc_seconds = struct.unpack_from("<QQ", raw, 20)
    stored_crc = struct.unpack_from("<I", raw, CRC_OFFSET)[0]
    calculated_crc = crc32(raw[:CRC_OFFSET])
    valid_header = raw[:4] == MAGIC and version == 1 and payload_length <= PAYLOAD_SIZE
    valid_crc = stored_crc == calculated_crc
    payload = raw[PAYLOAD_OFFSET : PAYLOAD_OFFSET + min(payload_length, PAYLOAD_SIZE)]
    return {
        "index": index,
        "sequence": sequence,
        "boot_id": boot_id,
        "event_id": event_id,
        "event": EVENTS.get(event_id, "unknown"),
        "payload_length": payload_length,
        "uptime_ms": uptime_ms,
        "rtc_seconds": rtc_seconds,
        "flags": flags,
        "crc_ok": valid_crc,
        "header_ok": valid_header,
        # Payloads are binary numeric layouts; hex is deterministic and cannot
        # accidentally turn a future string field into a credential leak.
        "payload_hex": payload.hex(),
        "fields": payload_fields(event_id, payload),
    }


def decode(path: Path) -> dict:
    data = path.read_bytes()
    full_length = len(data) - (len(data) % RECORD_SIZE)
    records = [decode_record(data[offset : offset + RECORD_SIZE], offset // RECORD_SIZE)
               for offset in range(0, full_length, RECORD_SIZE)]
    diagnostics = {
        "truncated_tail_bytes": len(data) - full_length,
        "crc_errors": sum(not record["crc_ok"] for record in records),
        "header_errors": sum(not record["header_ok"] for record in records),
        "unknown_events": sum(record["event"] == "unknown" for record in records),
        "unknown_enum_values": [],
        "sequence_gaps": [],
    }
    previous = None
    for record in records:
        if previous is not None and record["boot_id"] == previous["boot_id"]:
            expected = (previous["sequence"] + 1) & 0xFFFFFFFF
            if record["sequence"] != expected:
                diagnostics["sequence_gaps"].append(
                    {"boot_id": record["boot_id"], "expected": expected, "actual": record["sequence"]}
                )
        previous = record

        if record["header_ok"] and record["crc_ok"]:
            fields = record["fields"]
            for name, values in (("mode", ATTEMPT_MODES), ("origin", SELECTION_ORIGINS),
                                 ("trigger", FALLBACK_TRIGGERS), ("result", KOSYNC_RESULTS)):
                if name in fields and fields[name] not in values:
                    diagnostics["unknown_enum_values"].append(
                        {"index": record["index"], "event": record["event"], "field": name,
                         "value": fields[name]}
                    )
    return {"record_count": len(records), "diagnostics": diagnostics, "records": records}


def write_csv(result: dict) -> None:
    fields = [
        "index", "sequence", "boot_id", "event_id", "event", "payload_length", "uptime_ms",
        "rtc_seconds", "flags", "crc_ok", "header_ok", "payload_hex",
    ]
    output = csv.DictWriter(sys.stdout, fieldnames=fields, lineterminator="\n")
    output.writeheader()
    for record in result["records"]:
        output.writerow({field: record[field] for field in fields})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    parser.add_argument("--format", choices=("json", "csv"), default="json")
    args = parser.parse_args()
    try:
        result = decode(args.journal)
    except (OSError, ValueError, struct.error) as error:
        print(f"decode error: {error}", file=sys.stderr)
        return 2
    if args.format == "csv":
        write_csv(result)
    else:
        json.dump(result, sys.stdout, sort_keys=True, separators=(",", ":"))
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
