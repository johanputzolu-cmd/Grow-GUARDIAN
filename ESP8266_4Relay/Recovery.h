#pragma once

#include <stddef.h>
#include <stdint.h>

struct LegacyRecoveryState {
  uint32_t magic;
  uint32_t settingsHash;
  uint32_t temporaryEnd[4];
  uint32_t relayMask;
  uint32_t checksum;
};

struct RecoveryState {
  uint32_t magic;
  uint32_t settingsHash;
  uint32_t temporaryEnd[4];
  uint32_t relayMask;
  uint32_t conditionHash;
  uint32_t conditionEnd;
  uint32_t conditionFlags;
  uint32_t checksum;
};

inline uint32_t recoveryHash(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619UL;
  return hash;
}

inline bool validRecovery(const RecoveryState& state, uint32_t settingsHash) {
  return state.magic == 0x52535432 && state.settingsHash == settingsHash &&
      state.relayMask <= 15 && state.conditionFlags <= 7 &&
      state.checksum == recoveryHash(&state, offsetof(RecoveryState, checksum));
}

inline bool validLegacyRecovery(const LegacyRecoveryState& state, uint32_t settingsHash) {
  return state.magic == 0x52535431 && state.settingsHash == settingsHash &&
      state.relayMask <= 15 &&
      state.checksum == recoveryHash(&state, offsetof(LegacyRecoveryState, checksum));
}
