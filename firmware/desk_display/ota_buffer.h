#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace desk {
class OtaBuffer {
 public:
  void bind(uint8_t* storage, size_t size) { data=storage; capacity=size; used=0; }
  template<class Write> bool flush(Write&& write) {
    if (!used) return true;
    if (!write(data,used)) return false;
    used=0; return true;
  }
  template<class Write> bool append(const uint8_t* src,size_t len,Write&& write) {
    if (!data || !capacity) return false;
    while (len) {
      // Keep the final full buffer in RAM until request completion. Thus a
      // negotiated 16KB request is entirely received before flash is touched.
      if (used==capacity && !flush(write)) return false;
      const size_t n=len<capacity-used?len:capacity-used;
      memcpy(data+used,src,n);used+=n;src+=n;len-=n;
    }
    return true;
  }
 private:
  uint8_t* data=nullptr;
  size_t capacity=0,used=0;
};
}
