#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal helper to write a simple NDEF Text record into ST25DV64KC.
// For production, use ST's official libs or fully map the NDEF areas.

class ST25DV64KC {
public:
  explicit ST25DV64KC(TwoWire &w = Wire, uint8_t i2cAddr = 0x53)
  : _wire(&w), _addr(i2cAddr) {}

  bool begin();
  bool writeTextNDEF(const char* text, const char* lang = "en");

private:
  TwoWire* _wire;
  uint8_t _addr;

  bool writeBytes(uint16_t memAddr, const uint8_t* data, size_t len);
  // Very simplified: assumes NDEF user area starts at 0x0000 in EEPROM space.
  // Adjust to your board’s area (check your ST25DV64KC config).
  static const uint16_t NDEF_START = 0x0000;

  size_t buildTextRecord(uint8_t* buf, size_t maxLen, const char* lang, const char* text);
};
