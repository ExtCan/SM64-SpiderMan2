#ifdef _WIN32

#include "guard.h"

#include <atomic>
#include <cstdio>

#include "../common/log.h"

#if !defined(_MSC_VER)
// guard_x64.S: saves callee-saved state, records the stack pointer and a
// resume address in `buf`, calls fn(arg) and returns 0. After a fault the
// handler resumes at that address, which returns 1.
struct GuardBuf {
    uintptr_t rsp;
    uintptr_t resume;
};
extern "C" int sm2m_guarded_call(void (*fn)(void*), void* arg, GuardBuf* buf);
#endif

namespace sm2m::guard {
namespace {

// Win32 TLS (not thread_local): the handler runs on arbitrary game threads and
// must not allocate, which MinGW's emulated TLS can do on first access.
DWORD g_tlsBuf = TLS_OUT_OF_INDEXES;
DWORD g_tlsFault = TLS_OUT_OF_INDEXES;
DWORD g_tlsPhase = TLS_OUT_OF_INDEXES;
DWORD g_tlsDepth = TLS_OUT_OF_INDEXES;
PVOID g_veh = nullptr;
std::atomic<int> g_unguardedReported{0};

bool IsCrash(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_IN_PAGE_ERROR:
        return true;
    default:
        return false;
    }
}

void Fill(Fault* f, const EXCEPTION_RECORD* r) {
    if (!f) return;
    f->code = r->ExceptionCode;
    f->pc = uintptr_t(r->ExceptionAddress);
    f->access = -1;
    f->target = 0;
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        r->NumberParameters >= 2) {
        f->access = int(r->ExceptionInformation[0]);
        f->target = uintptr_t(r->ExceptionInformation[1]);
    }
    const char* ph = g_tlsPhase != TLS_OUT_OF_INDEXES ? static_cast<const char*>(TlsGetValue(g_tlsPhase)) : nullptr;
    f->phase = ph ? ph : "";
}

const char* CodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    default: return "exception";
    }
}

// Formats a fault without touching the heap.
void DescribeTo(const Fault& f, char* out, size_t n) {
    char where[192];
    DescribeAddress(f.pc, where, sizeof(where));
    const char* acc = f.access == 0 ? " reading" : f.access == 1 ? " writing" : f.access == 8 ? " executing" : "";
    if (f.access >= 0)
        std::snprintf(out, n, "%s%s 0x%llx at %s (code 0x%08lx) during '%s'", CodeName(f.code), acc,
                      static_cast<unsigned long long>(f.target), where, static_cast<unsigned long>(f.code), f.phase);
    else
        std::snprintf(out, n, "%s at %s (code 0x%08lx) during '%s'", CodeName(f.code), where,
                      static_cast<unsigned long>(f.code), f.phase);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsCrash(code) || g_tlsBuf == TLS_OUT_OF_INDEXES) return EXCEPTION_CONTINUE_SEARCH;
#if !defined(_MSC_VER)
    if (auto* buf = static_cast<GuardBuf*>(TlsGetValue(g_tlsBuf))) {
        Fill(static_cast<Fault*>(TlsGetValue(g_tlsFault)), ep->ExceptionRecord);
        TlsSetValue(g_tlsBuf, nullptr); // a fault while recovering is not ours
        ep->ContextRecord->Rsp = buf->rsp;
        ep->ContextRecord->Rip = buf->resume;
        ep->ContextRecord->Rax = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#else
    if (TlsGetValue(g_tlsDepth)) return EXCEPTION_CONTINUE_SEARCH; // the __except in Run handles it
#endif
    // Not in a guarded call. Leave a breadcrumb for the first few (the game
    // may handle it, or this is the crash) and let the normal chain run.
    if (g_unguardedReported.fetch_add(1) < 4) {
        Fault f;
        Fill(&f, ep->ExceptionRecord);
        char desc[400], line[1100];
        DescribeTo(f, desc, sizeof(desc));
        // Where the faulting address lives (freed memory = an unloaded DLL or
        // a released buffer; private executable memory = a hook trampoline or
        // generated code), and the first code addresses on the stack (who
        // called or jumped there).
        char mem[160] = "";
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(f.pc), &mbi, sizeof(mbi))) {
            const char* state = mbi.State == MEM_FREE ? "free" : mbi.State == MEM_RESERVE ? "reserved" : "committed";
            const char* type = mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : mbi.Type == MEM_PRIVATE ? "private" : "-";
            std::snprintf(mem, sizeof(mem), "; memory there: %s %s, protect 0x%lx, allocation %p", state, type,
                          static_cast<unsigned long>(mbi.Protect), mbi.AllocationBase);
        }
        char stack[480] = "";
        size_t used = 0;
        ULONG_PTR stackLow = 0, stackHigh = 0;
        GetCurrentThreadStackLimits(&stackLow, &stackHigh);
        const uintptr_t top = uintptr_t(stackHigh);
        const uintptr_t sp = uintptr_t(ep->ContextRecord->Rsp);
        int found = 0;
        for (int i = 0; i < 48 && found < 4 && sp && (sp & 7) == 0 && sp + uintptr_t(i + 1) * 8 <= top; ++i) {
            const uintptr_t v = reinterpret_cast<const uintptr_t*>(sp)[i];
            if (v < 0x10000) continue;
            HMODULE m = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(v), &m) || !m)
                continue;
            char where[192];
            DescribeAddress(v, where, sizeof(where));
            const int w = std::snprintf(stack + used, sizeof(stack) - used, "%s%s", found ? ", " : "; stack: ", where);
            if (w < 0 || size_t(w) >= sizeof(stack) - used) break;
            used += size_t(w);
            ++found;
        }
        std::snprintf(line, sizeof(line), "unguarded fault on thread %lu: %s%s%s", GetCurrentThreadId(), desc, mem, stack);
        log::Emergency(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#if defined(_MSC_VER)
int Filter(EXCEPTION_POINTERS* ep, Fault* f) {
    if (!IsCrash(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    Fill(f, ep->ExceptionRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool RunSeh(void (*fn)(void*), void* arg, Fault* fault) {
    __try {
        fn(arg);
        return true;
    } __except (Filter(GetExceptionInformation(), fault)) {
        return false;
    }
}
#endif

} // namespace

void Install() {
    if (g_veh) return;
    g_tlsFault = TlsAlloc();
    g_tlsPhase = TlsAlloc();
    g_tlsDepth = TlsAlloc();
    g_tlsBuf = TlsAlloc(); // last: the handler checks this one
    g_veh = AddVectoredExceptionHandler(1, Handler);
}

bool Installed() { return g_veh != nullptr; }

bool Run(void (*fn)(void*), void* arg, Fault* fault) {
    if (!g_veh) {
        fn(arg);
        return true;
    }
    // Locks taken by the code that faults are released below: recovery
    // doesn't unwind, so their LockGuards never run.
    const int locks = held_locks::Depth();
#if !defined(_MSC_VER)
    GuardBuf buf{0, 0};
    void* prevBuf = TlsGetValue(g_tlsBuf);
    void* prevFault = TlsGetValue(g_tlsFault);
    TlsSetValue(g_tlsFault, fault);
    TlsSetValue(g_tlsBuf, &buf);
    const int r = sm2m_guarded_call(fn, arg, &buf);
    TlsSetValue(g_tlsBuf, prevBuf);
    TlsSetValue(g_tlsFault, prevFault);
    if (r != 0) held_locks::ReleaseAbove(locks);
    return r == 0;
#else
    void* prevDepth = TlsGetValue(g_tlsDepth);
    TlsSetValue(g_tlsDepth, reinterpret_cast<void*>(1));
    const bool ok = RunSeh(fn, arg, fault);
    TlsSetValue(g_tlsDepth, prevDepth);
    if (!ok) held_locks::ReleaseAbove(locks);
    return ok;
#endif
}

void SetPhase(const char* phase) {
    if (g_tlsPhase != TLS_OUT_OF_INDEXES) TlsSetValue(g_tlsPhase, const_cast<char*>(phase));
}

const char* Phase() {
    if (g_tlsPhase == TLS_OUT_OF_INDEXES) return "";
    const char* p = static_cast<const char*>(TlsGetValue(g_tlsPhase));
    return p ? p : "";
}

void DescribeAddress(uintptr_t a, char* out, size_t n) {
    if (a < 0x10000) { // GetModuleHandleEx(FROM_ADDRESS, ~0) would report the exe
        std::snprintf(out, n, "0x%llx (null page)", static_cast<unsigned long long>(a));
        return;
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &mod) &&
        mod) {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < len; ++i)
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        char name8[128];
        if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, name8, sizeof(name8), nullptr, nullptr)) name8[0] = 0;
        std::snprintf(out, n, "%s+0x%llx", name8, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
    } else {
        std::snprintf(out, n, "0x%llx (no module)", static_cast<unsigned long long>(a));
    }
}

std::string Describe(const Fault& f) {
    char buf[400];
    DescribeTo(f, buf, sizeof(buf));
    return buf;
}

} // namespace sm2m::guard

#endif
