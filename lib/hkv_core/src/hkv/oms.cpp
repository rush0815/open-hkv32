#include "hkv/oms.h"

#include <math.h>
#include <string.h>

namespace hkv {
namespace oms {

namespace {

class Writer {
 public:
  Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
  void u8(uint8_t v) {
    if (len_ < cap_) buf_[len_] = v;
    ++len_;
  }
  void le(uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
  }
  size_t len() const { return len_; }
  bool ok() const { return len_ <= cap_; }
  uint8_t* data() { return buf_; }

 private:
  uint8_t* buf_;
  size_t cap_;
  size_t len_ = 0;
};

int16_t centiDegrees(float c) {
  float v = roundf(c * 100.0f);
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  return static_cast<int16_t>(v);
}

}  // namespace

uint16_t manufacturerCode(const char* l) {
  return static_cast<uint16_t>(((l[0] - 64) << 10) | ((l[1] - 64) << 5) | (l[2] - 64));
}

uint16_t encodeDateG(const DateTime& d) {
  const uint16_t y = static_cast<uint16_t>(d.year >= 2000 ? d.year - 2000 : 0) & 0x7F;
  return static_cast<uint16_t>((d.day & 0x1F) | ((y & 0x07) << 5) | ((d.month & 0x0F) << 8) |
                               ((y >> 3) << 12));
}

uint32_t encodeDateTimeF(const DateTime& d, bool valid) {
  const uint32_t y = static_cast<uint32_t>(d.year >= 2000 ? d.year - 2000 : 0) & 0x7F;
  uint32_t v = (d.minute & 0x3Fu) | ((d.hour & 0x1Fu) << 8) | ((d.day & 0x1Fu) << 16) |
               ((y & 0x07u) << 21) | ((d.month & 0x0Fu) << 24) | ((y >> 3) << 28);
  if (!valid) v |= 0x80u;      // IV: time invalid
  if (d.dst) v |= 0x8000u;     // SU: summer time
  return v;
}

size_t buildHcaTelegram(const HcaTelegram& t, IBlockCipher* cipher, uint8_t* out, size_t cap) {
  if (!out || !t.manufacturer) return 0;
  if (cap > 256) cap = 256;  // L-field is one byte
  if (t.security != 0 && t.security != 5) return 0;
  if (t.security == 5 && (!cipher || !t.aesKey)) return 0;

  Writer w(out, cap);
  w.u8(0);     // L, patched below
  w.u8(0x44);  // C: SND_NR
  w.le(manufacturerCode(t.manufacturer), 2);
  uint32_t id = t.meterId % 100000000u;
  for (int i = 0; i < 4; ++i) {  // 8 BCD digits, least significant byte first
    const uint8_t two = static_cast<uint8_t>(id % 100u);
    id /= 100u;
    w.u8(static_cast<uint8_t>(((two / 10) << 4) | (two % 10)));
  }
  w.u8(t.version);
  w.u8(kDeviceTypeHca);

  w.u8(0x7A);  // CI: short header
  w.u8(t.accessNo);
  w.u8(t.status);
  const size_t cfgPos = w.len();
  w.le(0, 2);  // configuration word, patched below

  const size_t payloadStart = w.len();
  if (t.security == 5) {
    w.u8(0x2F);  // decryption verification
    w.u8(0x2F);
  }
  if (t.timeValid) {
    w.u8(0x04);
    w.u8(0x6D);
    w.le(encodeDateTimeF(t.now, true), 4);
  }
  w.u8(0x03);
  w.u8(0x6E);
  w.le(t.currentUnits & 0xFFFFFFu, 3);
  if (t.dueDateValid) {
    w.u8(0x42);
    w.u8(0x6C);
    w.le(encodeDateG(t.dueDate), 2);
    w.u8(0x43);
    w.u8(0x6E);
    w.le(t.dueDateUnits & 0xFFFFFFu, 3);
  }
  if (t.radiatorValid) {
    w.u8(0x02);
    w.u8(0x59);
    w.le(static_cast<uint16_t>(centiDegrees(t.radiatorC)), 2);
  }
  if (t.roomValid) {
    w.u8(0x02);
    w.u8(0x65);
    w.le(static_cast<uint16_t>(centiDegrees(t.roomC)), 2);
  }

  uint16_t cfg = 0;
  if (t.security == 5) {
    while ((w.len() - payloadStart) % 16 != 0) w.u8(0x2F);  // idle filler
    const size_t blocks = (w.len() - payloadStart) / 16;
    if (blocks > 15 || !w.ok()) return 0;
    cfg = static_cast<uint16_t>(0x0500 | (blocks << 4));

    uint8_t iv[16];
    memcpy(iv, &out[2], 8);  // M (2) + A (6)
    memset(&iv[8], t.accessNo, 8);
    cipher->setKey(t.aesKey);
    uint8_t* p = &out[payloadStart];
    for (size_t b = 0; b < blocks; ++b, p += 16) {
      for (int i = 0; i < 16; ++i) p[i] ^= iv[i];
      cipher->encryptBlock(p, p);
      memcpy(iv, p, 16);
    }
  }
  if (!w.ok() || w.len() - 1 > 255) return 0;
  out[cfgPos] = static_cast<uint8_t>(cfg & 0xFF);
  out[cfgPos + 1] = static_cast<uint8_t>(cfg >> 8);
  out[0] = static_cast<uint8_t>(w.len() - 1);
  return w.len();
}

uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0;
  while (len--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x3D65) : static_cast<uint16_t>(crc << 1);
  }
  return static_cast<uint16_t>(~crc);
}

size_t toFrameFormatA(const uint8_t* frame, size_t len, uint8_t* out, size_t cap) {
  if (len < 10) return 0;
  size_t o = 0, i = 0;
  size_t block = 10;
  while (i < len) {
    const size_t n = (len - i) < block ? (len - i) : block;
    if (o + n + 2 > cap) return 0;
    memcpy(&out[o], &frame[i], n);
    const uint16_t c = crc16(&frame[i], n);
    o += n;
    out[o++] = static_cast<uint8_t>(c >> 8);
    out[o++] = static_cast<uint8_t>(c & 0xFF);
    i += n;
    block = 16;
  }
  return o;
}

size_t toHex(const uint8_t* data, size_t len, char* out, size_t cap) {
  static const char kHex[] = "0123456789ABCDEF";
  if (cap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; i < len && o + 2 < cap; ++i) {
    out[o++] = kHex[data[i] >> 4];
    out[o++] = kHex[data[i] & 0x0F];
  }
  out[o] = '\0';
  return o;
}

}  // namespace oms
}  // namespace hkv
