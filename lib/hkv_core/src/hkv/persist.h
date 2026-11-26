#pragma once
// Versioned, CRC protected blobs on top of IStorage.

#include <stddef.h>
#include <stdint.h>

#include "hkv/hal.h"

namespace hkv {

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

// Returns false (and leaves *obj untouched) if missing, wrong version/size or CRC mismatch.
bool loadBlob(IStorage& st, const char* key, uint16_t version, void* obj, size_t size);
bool saveBlob(IStorage& st, const char* key, uint16_t version, const void* obj, size_t size);

template <typename T>
bool loadObject(IStorage& st, const char* key, uint16_t version, T& obj) {
  return loadBlob(st, key, version, &obj, sizeof(T));
}
template <typename T>
bool saveObject(IStorage& st, const char* key, uint16_t version, const T& obj) {
  return saveBlob(st, key, version, &obj, sizeof(T));
}

}  // namespace hkv
