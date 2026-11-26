#include "hkv/log.h"

#include <stdarg.h>
#include <stdio.h>

namespace hkv {
namespace log {

static Sink g_sink = nullptr;

void setSink(Sink sink) { g_sink = sink; }

void printf(const char* fmt, ...) {
  if (!g_sink) return;
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  g_sink(line);
}

}  // namespace log
}  // namespace hkv
