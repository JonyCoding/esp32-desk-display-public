#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#define PROGMEM
inline float radians(float degrees) { return degrees * 0.01745329251994329577f; }
class __FlashStringHelper;
class String {
 public:
  explicit String(const char* s = "") : value(s) {}
  const char* c_str() const { return value.c_str(); }
  unsigned int length() const { return value.length(); }
 private:
  std::string value;
};
