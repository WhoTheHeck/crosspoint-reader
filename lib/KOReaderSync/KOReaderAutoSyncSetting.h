#pragma once

/** Shared persistence policy for the device and web-facing setting. */
struct KOReaderAutoSyncSetting final {
  static constexpr bool DEFAULT_ENABLED = false;

  static constexpr bool fromStoredValue(const bool isBoolean, const bool value) {
    return isBoolean ? value : DEFAULT_ENABLED;
  }
};
