#include "platform.h"

#include <algorithm>

#ifndef _WIN32
#include <chrono>
#endif

namespace sm2m {

double NowSeconds() {
#ifdef _WIN32
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(freq.QuadPart);
#else
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
#endif
}

#ifdef _WIN32
namespace held_locks {
namespace {
struct Stack {
    Mutex* held[64];
    int n;
};
// Win32 TLS (not thread_local): MinGW's emulated TLS allocates on first use.
DWORD g_tls = TLS_OUT_OF_INDEXES;
volatile LONG g_tlsInit = 0;

Stack* Get(bool create) {
    if (g_tls == TLS_OUT_OF_INDEXES) {
        if (InterlockedCompareExchange(&g_tlsInit, 1, 0) == 0) {
            g_tls = TlsAlloc();
            InterlockedExchange(&g_tlsInit, 2);
        } else {
            while (g_tlsInit != 2) YieldProcessor();
        }
        if (g_tls == TLS_OUT_OF_INDEXES) return nullptr;
    }
    auto* s = static_cast<Stack*>(TlsGetValue(g_tls));
    if (!s && create) {
        s = static_cast<Stack*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Stack)));
        if (s) TlsSetValue(g_tls, s);
    }
    return s;
}
} // namespace

void Push(Mutex* m) {
    Stack* s = Get(true);
    if (!s) return;
    if (s->n < 64) s->held[s->n] = m;
    ++s->n; // counted even past 64 so Pop stays balanced
}

void Pop(Mutex* m) {
    Stack* s = Get(false);
    if (!s || s->n <= 0) return;
    --s->n;
    if (s->n < 64 && s->held[s->n] != m) {
        // Not LIFO (shouldn't happen with scoped guards): drop m wherever it is.
        for (int i = std::min(s->n, 63); i >= 0; --i)
            if (s->held[i] == m) {
                s->held[i] = s->held[s->n];
                break;
            }
    }
}

int Depth() {
    Stack* s = Get(false);
    return s ? s->n : 0;
}

void ReleaseAbove(int depth) {
    Stack* s = Get(false);
    if (!s) return;
    while (s->n > depth && s->n > 0) {
        --s->n;
        if (s->n < 64 && s->held[s->n]) s->held[s->n]->unlock();
    }
}
} // namespace held_locks
#endif

} // namespace sm2m
