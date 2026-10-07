// Small portability layer. Everything outside src/win/, src/render/, src/input/,
// src/audio/ and src/game/sm2_* must compile on Linux too (unit tests run there).
#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <mutex>
#endif

namespace sm2m {

// Lightweight mutex. SRWLOCK on Windows so the DLL does not depend on the
// toolchain's std::thread implementation (MinGW win32 vs posix thread models).
class Mutex {
public:
    Mutex() {
#ifdef _WIN32
        InitializeSRWLock(&lock_);
#endif
    }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
#ifdef _WIN32
        AcquireSRWLockExclusive(&lock_);
#else
        m_.lock();
#endif
    }
    void unlock() {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&lock_);
#else
        m_.unlock();
#endif
    }
    bool try_lock() {
#ifdef _WIN32
        return TryAcquireSRWLockExclusive(&lock_) != 0;
#else
        return m_.try_lock();
#endif
    }

private:
#ifdef _WIN32
    SRWLOCK lock_;
#else
    std::mutex m_;
#endif
};

// The mutexes the current thread holds through LockGuard. The crash guard
// resumes after a fault without unwinding the stack, so it uses this to
// release the locks the faulting code had taken (else the next thread to want
// one would wait forever - a frozen game).
namespace held_locks {
#ifdef _WIN32
void Push(Mutex* m);
void Pop(Mutex* m);
int Depth();
// Unlocks (newest first) everything taken since Depth() returned `depth`.
void ReleaseAbove(int depth);
#else
inline void Push(Mutex*) {}
inline void Pop(Mutex*) {}
inline int Depth() { return 0; }
inline void ReleaseAbove(int) {}
#endif
} // namespace held_locks

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) {
        m_.lock();
        held_locks::Push(&m_);
    }
    ~LockGuard() {
        held_locks::Pop(&m_);
        m_.unlock();
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_;
};

// Monotonic seconds.
double NowSeconds();

} // namespace sm2m
