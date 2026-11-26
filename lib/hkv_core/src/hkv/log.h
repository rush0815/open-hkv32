#pragma once
// Minimal logging hook. The port registers a sink (Serial, UART, RTT, ...).

namespace hkv {
namespace log {

using Sink = void (*)(const char* line);

void setSink(Sink sink);
void printf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace log
}  // namespace hkv

#define HKV_LOG(...) ::hkv::log::printf(__VA_ARGS__)
