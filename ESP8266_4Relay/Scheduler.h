#pragma once

#include <stdint.h>
#include <time.h>

struct ScheduleSlot {
  uint8_t enabled;
  uint8_t days;
  uint8_t scheduleType;
  uint16_t durationMinute;
  uint16_t intervalMinute;
  uint16_t startMinute;
  uint16_t stopMinute;
};

struct WeeklyProgram {
  uint8_t mode;
  uint8_t channel;
  ScheduleSlot slots[4];
};

inline bool validSlot(const ScheduleSlot& slot) {
  return slot.enabled <= 1 && slot.days <= 127 &&
      (!slot.enabled || slot.days != 0) && slot.scheduleType <= 1 &&
      slot.durationMinute >= 1 && slot.durationMinute <= 1440 &&
      slot.intervalMinute >= slot.durationMinute && slot.intervalMinute <= 1440 &&
      slot.startMinute < 1440 && slot.stopMinute < 1440;
}

inline bool slotState(const struct tm& local, const ScheduleSlot& slot) {
  if (!slot.enabled) return false;
  const uint16_t minute = local.tm_hour * 60 + local.tm_min;
  const uint16_t length = slot.stopMinute == slot.startMinute ? 1440 :
      (slot.stopMinute + 1440 - slot.startMinute) % 1440;
  const uint16_t elapsed = (minute + 1440 - slot.startMinute) % 1440;
  if (elapsed >= length) return false;
  // A night window belongs to the weekday on which it started (Monday = bit 0).
  int day = (local.tm_wday + 6) % 7;
  if (minute < slot.startMinute) day = (day + 6) % 7;
  if (!(slot.days & (1 << day))) return false;
  return slot.scheduleType == 0 || elapsed % slot.intervalMinute < slot.durationMinute;
}

inline bool weeklyState(const struct tm& local, const WeeklyProgram& program) {
  for (const ScheduleSlot& slot : program.slots) {
    if (slotState(local, slot)) return true;
  }
  return false;
}

inline bool temporaryActive(uint32_t now, uint32_t start, uint32_t duration) {
  return duration != 0 && static_cast<uint32_t>(now - start) < duration;
}
