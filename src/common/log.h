#pragma once

#include <cstdarg>
#include <string>

namespace sm2m::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Opens (truncates) the log file. Safe to call once; later calls are ignored.
void Init(const std::string& path, Level minLevel);
void SetMinLevel(Level level);
#if defined(__MINGW32__)
#define SM2M_PRINTF_ATTR __attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
#define SM2M_PRINTF_ATTR __attribute__((format(printf, 2, 3)))
#else
#define SM2M_PRINTF_ATTR
#endif
void Write(Level level, const char* fmt, ...) SM2M_PRINTF_ATTR;
// Lock-free, allocation-free line write for use inside exception handlers.
void Emergency(const char* msg);
void Flush();

} // namespace sm2m::log

#define LOGD(...) ::sm2m::log::Write(::sm2m::log::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::sm2m::log::Write(::sm2m::log::Level::Info, __VA_ARGS__)
#define LOGW(...) ::sm2m::log::Write(::sm2m::log::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::sm2m::log::Write(::sm2m::log::Level::Error, __VA_ARGS__)
