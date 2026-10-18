#include <cassert>
#include <iostream>
#include "Condition.h"

int main() {
  ConditionConfig config = {1, 2, 1, 1, 0, 0, 0, 0, 280, 700, 1, 0};
  assert(validCondition(config));
  auto invalid = config;
  invalid.channel = 0; assert(!validCondition(invalid));
  invalid = config; invalid.temperatureLimit = 801; assert(!validCondition(invalid));
  invalid = config; invalid.humidityLimit = 1001; assert(!validCondition(invalid));
  invalid = config; invalid.durationMinute = 0; assert(!validCondition(invalid));
  invalid = config; invalid.logic = 2; assert(!validCondition(invalid));
  invalid = config; invalid.temperatureEnabled = invalid.humidityEnabled = 0;
  assert(!validCondition(invalid)); invalid.enabled = 0; assert(validCondition(invalid));

  ConditionSample sample;
  sample.received = sample.temperatureValid = sample.airValid = sample.soilValid = true;
  sample.receivedAt = 100;
  sample.temperature = 280;
  sample.air = 700;
  sample.soil = 900;
  ConditionRuntime state;
  updateCondition(config, sample, 100, true, state);
  assert(!state.demand); // Strictly greater, not greater-or-equal.
  sample.temperature = 281;
  updateCondition(config, sample, 101, true, state);
  assert(state.demand);
  sample.temperature = 279;
  updateCondition(config, sample, 102, true, state);
  assert(state.demand); // Hysteresis.
  sample.temperature = 275;
  updateCondition(config, sample, 103, true, state);
  assert(!state.demand);
  sample.air = 701;
  updateCondition(config, sample, 104, true, state);
  assert(state.demand);
  sample.air = 690;
  updateCondition(config, sample, 105, true, state);
  assert(state.demand);
  sample.air = 680;
  updateCondition(config, sample, 106, true, state);
  assert(!state.demand);

  config.logic = 1;
  state = {};
  sample.temperature = 290;
  updateCondition(config, sample, 107, true, state);
  assert(!state.demand);
  sample.air = 710;
  updateCondition(config, sample, 108, true, state);
  assert(state.demand);
  config.temperatureEnabled = 0;
  sample.temperatureValid = false;
  state = {};
  updateCondition(config, sample, 109, true, state);
  assert(state.demand); // AND with only one enabled threshold.
  config.humiditySource = 1;
  sample.airValid = false;
  sample.soil = 650;
  state = {};
  updateCondition(config, sample, 110, true, state);
  assert(!state.demand);
  sample.soil = 750;
  updateCondition(config, sample, 111, true, state);
  assert(state.demand); // Soil is independent from invalid DHT readings.
  updateCondition(config, sample, 30099, true, state);
  assert(state.demand);
  updateCondition(config, sample, 30100, true, state);
  assert(!state.fresh && !state.demand); // Exact 30-second TTL.
  sample.receivedAt = 30101;
  updateCondition(config, sample, 30101, true, state);
  assert(state.demand);
  sample.soilValid = false;
  updateCondition(config, sample, 30102, true, state);
  assert(!state.demand); // Explicit sensor faults cancel immediately.
  sample.soilValid = true;

  config.action = 1;
  state = {};
  sample.receivedAt = 40000;
  updateCondition(config, sample, 40000, true, state);
  assert(state.demand && conditionRemaining(state, 40000) == 60);
  for (uint32_t now = 45000; now <= 95000; now += 5000) {
    sample.receivedAt = now;
    updateCondition(config, sample, now, true, state);
    assert(state.demand);
  }
  sample.receivedAt = 100000;
  updateCondition(config, sample, 100000, true, state);
  assert(!state.demand && conditionRemaining(state, 100000) == 0);
  sample.receivedAt = 110000;
  updateCondition(config, sample, 110000, true, state);
  assert(!state.demand); // No indefinite retrigger while high.
  sample.soil = 680;
  updateCondition(config, sample, 110001, true, state);
  sample.soil = 701;
  updateCondition(config, sample, 110002, true, state);
  assert(state.demand);
  updateCondition(config, sample, 140000, true, state);
  assert(!state.demand); // Loss cancels even a still-running timer.
  sample.receivedAt = 140001;
  updateCondition(config, sample, 140001, true, state);
  assert(!state.demand); // Reconnection alone cannot rearm a timed run.
  sample.soil = 680;
  updateCondition(config, sample, 140002, true, state);
  sample.soil = 701;
  updateCondition(config, sample, 140003, true, state);
  assert(state.demand);
  updateCondition(config, sample, 140004, false, state);
  assert(!state.demand && !state.matched);

  state = {};
  sample.receivedAt = UINT32_MAX - 10000;
  updateCondition(config, sample, sample.receivedAt, true, state);
  assert(state.demand);
  const uint32_t start = sample.receivedAt;
  sample.receivedAt = start + 20000;
  updateCondition(config, sample, start + 20000, true, state);
  assert(state.demand && conditionRemaining(state, start + 20000) == 40);
  sample.receivedAt = start + 60000;
  updateCondition(config, sample, start + 60000, true, state);
  assert(!state.demand);
  std::cout << "PASS: condition validation, strict thresholds, hysteresis, ET/OU, "
               "single sensor, air/soil, TTL, invalid sensors, timed crossings, "
               "no repeated triggers, disconnect cancellation, PROG gating and rollover\n";
}
