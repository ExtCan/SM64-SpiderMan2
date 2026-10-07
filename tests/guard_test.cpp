// Exercises the crash guard under real faults (run on Windows or Wine).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/common/log.h"
#include "../src/common/platform.h"
#include "../src/win/guard.h"

using namespace sm2m;

extern "C" void test_pushrbx_func(void*);    // 40 53 / mov rax,rcx / pop rbx / ret
extern "C" void test_clobber_and_fault(void*); // trashes callee-saved regs, then faults

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("  FAIL line %d: %s\n", __LINE__, #c);                \
        }                                                                     \
    } while (0)

__attribute__((noinline)) static void NullWrite(void*) {
    volatile int* p = reinterpret_cast<volatile int*>(0x10);
    *p = 42;
}
__attribute__((noinline)) static void Fine(void* arg) { *static_cast<int*>(arg) += 1; }

// The game handling a fault itself (a vectored handler of its own, called
// after the mod's): skips the faulting call by returning to its caller.
static LONG CALLBACK GameHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    c->Rip = *reinterpret_cast<DWORD64*>(c->Rsp); // the jump target faulted at entry: [rsp] is the return address
    c->Rsp += 8;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main() {
    log::Init("guard_test.log", log::Level::Debug);
    guard::Install();

    // 1) Plain access violation.
    guard::Fault f;
    {
        guard::PhaseScope ph("null write");
        CHECK(!guard::Run(NullWrite, nullptr, &f));
    }
    CHECK(f.code == EXCEPTION_ACCESS_VIOLATION && f.access == 1 && f.target == 0x10);
    CHECK(std::strcmp(f.phase, "null write") == 0);
    std::printf("1: %s\n", guard::Describe(f).c_str());

    // 2) The crash from the field: entering a function 2 bytes past its
    //    `push rbx`, so its ret pops a bogus return address.
    {
        guard::PhaseScope ph("mid-function call");
        void (*mid)(void*) = reinterpret_cast<void (*)(void*)>(reinterpret_cast<uintptr_t>(&test_pushrbx_func) + 2);
        CHECK(!guard::Run(mid, nullptr, &f));
    }
    std::printf("2: %s\n", guard::Describe(f).c_str());
    CHECK(f.code == EXCEPTION_ACCESS_VIOLATION && f.access == 8 && f.target == 0);
    // ...and calling the real start works.
    CHECK(guard::Run(test_pushrbx_func, nullptr, &f));

    // 3) Callee-saved registers survive a fault that trashed them.
    volatile int keep = 1234;
    double dkeep = 3.25;
    for (int i = 0; i < 3; ++i) CHECK(!guard::Run(test_clobber_and_fault, nullptr, &f));
    CHECK(keep == 1234 && dkeep == 3.25);
    std::printf("3: %s\n", guard::Describe(f).c_str());

    // 4) Nesting: an inner fault is caught by the inner guard only.
    int counter = 0;
    auto outer = [&] {
        guard::Fault inner;
        CHECK(!guard::Run(NullWrite, nullptr, &inner));
        Fine(&counter);
    };
    CHECK(guard::Call(outer, &f));
    CHECK(counter == 1);

    // 5) Many faults in a row: no drift.
    int ok = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!guard::Run(NullWrite, nullptr, &f)) ++ok;
        Fine(&counter);
    }
    CHECK(ok == 1000 && counter == 1001);

    // 6) Non-faulting calls still return true and run.
    CHECK(guard::Run(Fine, &counter, &f) && counter == 1002);

    // 7) Locks taken by code that faults are released (recovery doesn't
    //    unwind, so the LockGuards never ran): no deadlock on the next use.
    Mutex a, b;
    {
        LockGuard outerLock(a); // held before the guarded call: must stay held
        auto body = [&] {
            LockGuard inner(b);
            NullWrite(nullptr);
        };
        CHECK(!guard::Call(body, &f));
        CHECK(held_locks::Depth() == 1);
        const bool free = b.try_lock();
        CHECK(free);
        if (free) b.unlock();
        CHECK(!a.try_lock()); // still held by outerLock
    }
    CHECK(held_locks::Depth() == 0);
    CHECK(a.try_lock());
    a.unlock();
    // ...and a lock taken and released normally inside a guarded call is fine.
    auto clean = [&] { LockGuard g(b); };
    CHECK(guard::Call(clean, &f) && held_locks::Depth() == 0 && b.try_lock());
    b.unlock();

    // 6) A fault outside any guarded call that the game handles itself: the
    //    mod only leaves a breadcrumb - what the memory is and who called.
    {
        PVOID h = AddVectoredExceptionHandler(0, GameHandler);
        void* freed = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        VirtualFree(freed, 0, MEM_RELEASE);
        auto call = reinterpret_cast<void (*)()>(freed);
        call(); // executes freed memory; the game's handler returns here
        RemoveVectoredExceptionHandler(h);
        log::Flush();
        FILE* lf = std::fopen("guard_test.log", "rb");
        std::string text;
        if (lf) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), lf)) > 0) text.append(buf, n);
            std::fclose(lf);
        }
        const size_t at = text.find("unguarded fault");
        CHECK(at != std::string::npos);
        const std::string line = at == std::string::npos ? "" : text.substr(at, text.find('\n', at) - at);
        std::printf("6: %s\n", line.c_str());
        CHECK(line.find("memory there: free") != std::string::npos);
        CHECK(line.find("stack: guard_test.exe+") != std::string::npos);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
