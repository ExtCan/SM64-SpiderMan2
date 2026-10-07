// A small seqlock over 32-bit words, for a handful of values one thread
// publishes and another reads without a lock: the mod's Present thread and
// the game's own thread (inside its engine calls, hooked). Readers retry
// while a write is in progress; writers take turns through the sequence
// number. Unit tested with real threads (tests/test_main.cpp).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define SM2M_CPU_PAUSE() _mm_pause()
#else
#define SM2M_CPU_PAUSE() ((void)0)
#endif

namespace sm2m {

template <size_t N>
struct SeqWords {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> w[N];

    SeqWords() {
        for (auto& x : w) x.store(0, std::memory_order_relaxed);
    }

    // False: another writer held it for very long (never expected) - this
    // write is dropped rather than waited for.
    bool Write(const uint32_t* v) {
        uint32_t s = seq.load(std::memory_order_relaxed);
        for (int spin = 0;; ++spin) {
            if (!(s & 1u) && seq.compare_exchange_weak(s, s + 1, std::memory_order_acquire)) break;
            if (spin > 4096) return false;
            SM2M_CPU_PAUSE();
            s = seq.load(std::memory_order_relaxed);
        }
        // (the odd sequence number before any of the words)
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < N; ++i) w[i].store(v[i], std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);
        return true;
    }

    // All N words from one write (and its sequence number, which changes
    // with every write; 0 = never written). False if writes kept it busy.
    bool Read(uint32_t* v, uint32_t* seqOut = nullptr) const {
        for (int tries = 0; tries < 256; ++tries) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1u) {
                SM2M_CPU_PAUSE(); // a write in progress (a dozen stores)
                continue;
            }
            for (size_t i = 0; i < N; ++i) v[i] = w[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1) {
                if (seqOut) *seqOut = s1;
                return true;
            }
        }
        return false;
    }
};

} // namespace sm2m
