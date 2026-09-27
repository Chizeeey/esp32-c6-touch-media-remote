// Minimal CST816 capacitive touch driver.
//
// The chip exposes a 7-byte status block at register 0x00:
//   [0] unused  [1] gesture  [2] finger count  [3..4] X (12-bit)  [5..6] Y (12-bit)
#pragma once

#include <Arduino.h>
#include <Wire.h>

class CST816 {
 public:
  bool begin(TwoWire &wire, uint8_t addr) {
    wire_ = &wire;
    addr_ = addr;
    if (!present()) return false;
    writeReg(0x00, 0x00);  // leave gesture-only mode, report coordinates
    return true;
  }

  bool present() {
    wire_->beginTransmission(addr_);
    return wire_->endTransmission() == 0;
  }

  // Returns true while a finger is down, writing panel-native coordinates.
  bool read(uint16_t &x, uint16_t &y) {
    uint8_t b[7];
    if (!readRegs(0x00, b, sizeof(b))) return false;
    if (b[2] == 0) return false;
    x = (uint16_t(b[3] & 0x0F) << 8) | b[4];
    y = (uint16_t(b[5] & 0x0F) << 8) | b[6];
    return true;
  }

 private:
  TwoWire *wire_ = nullptr;
  uint8_t addr_ = 0;

  void writeReg(uint8_t reg, uint8_t val) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(val);
    wire_->endTransmission();
  }

  bool readRegs(uint8_t reg, uint8_t *buf, size_t len) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom(addr_, uint8_t(len)) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = wire_->read();
    return true;
  }
};
