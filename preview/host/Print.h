#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) = 0;
  size_t write(const uint8_t* data, size_t count) { size_t n = 0; while (n < count) { write(data[n]); ++n; } return n; }
  size_t write(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), std::strlen(text)); }
  size_t print(const char* text) { return write(text); }
};
