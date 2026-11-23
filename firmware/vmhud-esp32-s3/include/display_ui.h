#pragma once

#include <Arduino.h>

struct DisplayState {
  bool bleConnected = false;
  bool hudInfoOk = false;
  bool notifySubscribed = false;
  uint8_t postedLimit = 0;
  uint16_t camDist = 0;
  uint8_t camType = 0;
  uint16_t currentSpeed = 0;
  String currentRoad = "";
  uint8_t lastEmittedLimit = 0;
  uint32_t splimTxCount = 0;
  uint32_t lastSplimMs = 0;
  uint32_t camPktCount = 0;
  uint32_t navPktCount = 0;
  uint32_t hbPktCount = 0;
  uint32_t uptimeSec = 0;
};

#if defined(VMHUD_HAS_TFT) || defined(VMHUD_HAS_OLED)
#define VMHUD_HAS_DISPLAY 1
void displayInit();
void displayUpdate(const DisplayState &state, bool force = false);
#else
#define VMHUD_HAS_DISPLAY 0
inline void displayInit() {}
inline void displayUpdate(const DisplayState &, bool = false) {}
#endif
