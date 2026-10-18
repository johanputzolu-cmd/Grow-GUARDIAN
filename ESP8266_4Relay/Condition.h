#pragma once

#include <stdint.h>

struct ConditionConfig {
  uint8_t enabled;
  uint8_t channel;
  uint8_t temperatureEnabled;
  uint8_t humidityEnabled;
  uint8_t humiditySource;  // 0 = air, 1 = soil.
  uint8_t logic;           // 0 = OR, 1 = AND.
  uint8_t action;          // 0 = while exceeded, 1 = timed crossing.
  uint8_t reserved;
  int16_t temperatureLimit; // Tenths of a degree or percent.
  uint16_t humidityLimit;
  uint16_t durationMinute;
  uint16_t padding;
};

inline bool validCondition(const ConditionConfig& config) {
  return config.enabled <= 1 && config.channel >= 1 && config.channel <= 4 &&
      config.temperatureEnabled <= 1 && config.humidityEnabled <= 1 &&
      (!config.enabled || config.temperatureEnabled || config.humidityEnabled) &&
      config.humiditySource <= 1 && config.logic <= 1 && config.action <= 1 &&
      config.temperatureLimit >= -400 && config.temperatureLimit <= 800 &&
      config.humidityLimit <= 1000 &&
      config.durationMinute >= 1 && config.durationMinute <= 1440 &&
      config.reserved == 0 && config.padding == 0;
}

struct ConditionSample {
  bool received = false;
  bool temperatureValid = false;
  bool airValid = false;
  bool soilValid = false;
  int16_t temperature = 0;
  uint16_t air = 0;
  uint16_t soil = 0;
  uint32_t receivedAt = 0;
};

struct ConditionRuntime {
  bool temperatureHigh = false;
  bool humidityHigh = false;
  bool matched = false;
  bool fresh = false;
  bool demand = false;
  uint32_t startedAt = 0;
  uint32_t durationMs = 0;
};

inline bool conditionSampleFresh(const ConditionConfig& config, const ConditionSample& sample, uint32_t now) {
  return sample.received && static_cast<uint32_t>(now - sample.receivedAt) < 30000 &&
      (!config.temperatureEnabled || sample.temperatureValid) &&
      (!config.humidityEnabled || (config.humiditySource == 0 ? sample.airValid : sample.soilValid));
}

inline bool thresholdHigh(int value, int limit, int margin, bool previous) {
  return previous ? value > limit - margin : value > limit;
}

inline void updateCondition(const ConditionConfig& config, const ConditionSample& sample,
                            uint32_t now, bool programMode, ConditionRuntime& state) {
  if (!config.enabled || !programMode) {
    state = {};
    return;
  }
  state.fresh = conditionSampleFresh(config, sample, now);
  if (!state.fresh) {
    state.demand = false;
    state.durationMs = 0;
    // Preserve crossing latches: reconnecting must not repeatedly restart a timed run.
    return;
  }
  state.temperatureHigh = config.temperatureEnabled &&
      thresholdHigh(sample.temperature, config.temperatureLimit, 5, state.temperatureHigh);
  state.humidityHigh = config.humidityEnabled &&
      thresholdHigh(config.humiditySource == 0 ? sample.air : sample.soil,
                    config.humidityLimit, 20, state.humidityHigh);
  const bool matched = config.logic == 1
      ? (!config.temperatureEnabled || state.temperatureHigh) &&
        (!config.humidityEnabled || state.humidityHigh)
      : state.temperatureHigh || state.humidityHigh;
  if (config.action == 0) {
    state.demand = matched;
  } else {
    const bool running = state.durationMs != 0 &&
        static_cast<uint32_t>(now - state.startedAt) < state.durationMs;
    if (!running) state.durationMs = 0;
    if (matched && !state.matched && !running) {
      state.startedAt = now;
      state.durationMs = static_cast<uint32_t>(config.durationMinute) * 60000UL;
    }
    state.demand = state.durationMs != 0 &&
        static_cast<uint32_t>(now - state.startedAt) < state.durationMs;
  }
  state.matched = matched;
}

inline uint32_t conditionRemaining(const ConditionRuntime& state, uint32_t now) {
  if (!state.demand || state.durationMs == 0) return 0;
  const uint32_t elapsed = static_cast<uint32_t>(now - state.startedAt);
  return elapsed < state.durationMs ? (state.durationMs - elapsed + 999) / 1000 : 0;
}
