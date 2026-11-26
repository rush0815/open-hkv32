#pragma once
// Compact software AES-128 (encryption only; CBC encryption needs no decryption).
// A port can replace it with a hardware implementation of IBlockCipher.

#include "hkv/hal.h"

namespace hkv {

class SoftAes128 : public IBlockCipher {
 public:
  void setKey(const uint8_t key[16]) override;
  void encryptBlock(const uint8_t in[16], uint8_t out[16]) override;

 private:
  uint8_t roundKeys_[176] = {};
};

}  // namespace hkv
