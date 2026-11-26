#include "hkv/persist.h"

#include <string.h>

namespace hkv {

namespace {
constexpr uint32_t kMagic = 0x484B5631u;  // "HKV1"
constexpr size_t kMaxBlob = 512;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t crc;
};
}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool loadBlob(IStorage& st, const char* key, uint16_t version, void* obj, size_t size) {
  if (size > kMaxBlob) return false;
  uint8_t buf[sizeof(Header) + kMaxBlob];
  if (!st.read(key, buf, sizeof(Header) + size)) return false;
  Header h;
  memcpy(&h, buf, sizeof(h));
  if (h.magic != kMagic || h.version != version || h.size != size) return false;
  if (crc32(buf + sizeof(Header), size) != h.crc) return false;
  memcpy(obj, buf + sizeof(Header), size);
  return true;
}

bool saveBlob(IStorage& st, const char* key, uint16_t version, const void* obj, size_t size) {
  if (size > kMaxBlob) return false;
  uint8_t buf[sizeof(Header) + kMaxBlob];
  Header h{kMagic, version, static_cast<uint16_t>(size), crc32(obj, size)};
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(Header), obj, size);
  return st.write(key, buf, sizeof(Header) + size);
}

}  // namespace hkv
