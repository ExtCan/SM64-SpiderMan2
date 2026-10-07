#include "log.h"

#include "platform.h"

#include <cstdio>
#include <ctime>

namespace sm2m::log {
namespace {
Mutex g_mutex;
Level g_min = Level::Info;
bool g_initialized = false;
#ifdef _WIN32
// One append-only Win32 handle: every write lands at the end of the file, so
// lines from the crash handler (Emergency) and normal logging never overwrite
// each other, and no C runtime locks are involved.
HANDLE g_handle = INVALID_HANDLE_VALUE;
#else
FILE* g_file = nullptr;
#endif

const char* LevelName(Level l) {
    switch (l) {
    case Level::Debug: return "DBG";
    case Level::Info: return "INF";
    case Level::Warn: return "WRN";
    case Level::Error: return "ERR";
    }
    return "???";
}

void RawWrite(const char* s, size_t n) {
#ifdef _WIN32
    if (g_handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_handle, s, DWORD(n), &written, nullptr);
#else
    FILE* f = g_file ? g_file : stderr;
    std::fwrite(s, 1, n, f);
    std::fflush(f);
#endif
}
} // namespace

void Init(const std::string& path, Level minLevel) {
    LockGuard lock(g_mutex);
    g_min = minLevel;
    if (g_initialized) return;
    g_initialized = true;
#ifdef _WIN32
    // UTF-8 path -> wide, so folders with non-ASCII names work.
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);
    // Truncate, then reopen append-only.
    HANDLE t = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    g_handle = CreateFileW(wpath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    g_file = std::fopen(path.c_str(), "w");
#endif
}

void SetMinLevel(Level level) {
    LockGuard lock(g_mutex);
    g_min = level;
}

void Write(Level level, const char* fmt, ...) {
    if (level < g_min) return;
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const double t = NowSeconds();
    char line[2200];
    const int n = std::snprintf(line, sizeof(line), "[%10.3f][%s] %s\n", t, LevelName(level), msg);
    if (n <= 0) return;

    LockGuard lock(g_mutex);
    RawWrite(line, size_t(n) < sizeof(line) ? size_t(n) : sizeof(line) - 1);
#ifdef _WIN32
    OutputDebugStringA(line);
#endif
}

void Emergency(const char* msg) {
    // No lock, no heap, no floating point formatting: this runs inside the
    // vectored exception handler on whatever thread faulted.
    char line[600];
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    const unsigned long long ms = f.QuadPart ? (unsigned long long)(c.QuadPart / (f.QuadPart / 1000)) : 0ull;
#else
    const unsigned long long ms = (unsigned long long)(NowSeconds() * 1000.0);
#endif
    const int n = std::snprintf(line, sizeof(line), "[%6llu.%03llu][ERR] %s\n", ms / 1000ull, ms % 1000ull, msg);
    if (n > 0) RawWrite(line, size_t(n) < sizeof(line) ? size_t(n) : sizeof(line) - 1);
}

void Flush() {
#ifndef _WIN32
    LockGuard lock(g_mutex);
    if (g_file) std::fflush(g_file);
#endif
}

} // namespace sm2m::log
