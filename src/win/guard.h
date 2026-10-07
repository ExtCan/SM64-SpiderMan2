// Crash guard: runs a piece of code so that a hardware fault inside it
// (access violation, illegal instruction, ...) returns control to the mod
// instead of taking the game down. Used around every call into game code and
// around the whole per-frame update, so a wrong signature or offset in a new
// game version turns into a log line instead of a crash.
//
// MinGW builds use a vectored exception handler plus a small assembly stub
// (guard_x64.S); MSVC builds use __try/__except.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>

#include "../common/platform.h"

namespace sm2m::guard {

struct Fault {
    DWORD code = 0;
    uintptr_t pc = 0;     // faulting instruction
    uintptr_t target = 0; // address accessed (access violations)
    int access = -1;      // 0 read, 1 write, 8 execute
    const char* phase = "";
};

// Installs the handler. Call once, early, before any guarded call.
void Install();
bool Installed();

// Runs fn(arg). Returns false if it faulted (and fills `fault` if given).
bool Run(void (*fn)(void*), void* arg, Fault* fault);

template <typename F>
bool Call(F& f, Fault* fault) {
    return Run([](void* p) { (*static_cast<F*>(p))(); }, &f, fault);
}

// What the current thread is doing, reported with faults.
void SetPhase(const char* phase);
const char* Phase();

class PhaseScope {
public:
    explicit PhaseScope(const char* phase) : prev_(Phase()) { SetPhase(phase); }
    ~PhaseScope() { SetPhase(prev_); }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

private:
    const char* prev_;
};

// "access violation reading 0x10 at Spider-Man2.exe+0x49ce5a0 during 'hero SetPosition'"
std::string Describe(const Fault& f);
// "Spider-Man2.exe+0x1234" (no allocation; safe inside the exception handler)
void DescribeAddress(uintptr_t a, char* out, size_t n);

} // namespace sm2m::guard

#endif
