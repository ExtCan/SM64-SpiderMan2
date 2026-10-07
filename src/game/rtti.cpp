#include "rtti.h"

#include <cstring>

namespace sm2m {
namespace rtti {
namespace {

const uint8_t* FindBytes(const uint8_t* hay, size_t n, const uint8_t* needle, size_t m) {
    if (m == 0 || n < m) return nullptr;
    const uint8_t* end = hay + (n - m) + 1;
    for (const uint8_t* p = hay; p < end;) {
        const void* hit = std::memchr(p, needle[0], size_t(end - p));
        if (!hit) return nullptr;
        const uint8_t* h = static_cast<const uint8_t*>(hit);
        if (std::memcmp(h, needle, m) == 0) return h;
        p = h + 1;
    }
    return nullptr;
}

uint32_t U32(uintptr_t a) {
    uint32_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
    return v;
}

bool Inside(const std::vector<Span>& data, uintptr_t a, size_t n) {
    for (const Span& s : data)
        if (a >= s.addr && a + n <= s.addr + s.size) return true;
    return false;
}

} // namespace

uintptr_t FindVtable(uintptr_t base, const std::vector<Span>& data, const char* mangled, uint32_t offset) {
    const size_t len = std::strlen(mangled) + 1; // with the terminating zero
    const uint8_t* name = reinterpret_cast<const uint8_t*>(mangled);
    for (const Span& ns : data) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ns.addr);
        size_t left = ns.size;
        while (const uint8_t* hit = FindBytes(p, left, name, len)) {
            const uintptr_t td = reinterpret_cast<uintptr_t>(hit) - 16;
            p = hit + 1;
            left = ns.size - size_t(p - reinterpret_cast<const uint8_t*>(ns.addr));
            if ((td & 7) != 0 || td < base || td - base > 0xFFFFFFFFull || !Inside(data, td, 16)) continue;
            const uint32_t tdRva = uint32_t(td - base);
            // Locators that name this type descriptor (dword at +12).
            for (const Span& cs : data) {
                const uintptr_t first = (cs.addr + 3) & ~uintptr_t(3);
                for (uintptr_t a = first; a + 4 <= cs.addr + cs.size; a += 4) {
                    if (U32(a) != tdRva) continue;
                    const uintptr_t col = a - 12;
                    if (col < cs.addr || !Inside(data, col, 24)) continue;
                    if (U32(col) != 1 || U32(col + 4) != offset || col - base != U32(col + 20)) continue;
                    // The vtable starts right after a pointer to the locator.
                    for (const Span& vs : data) {
                        const uintptr_t vfirst = (vs.addr + 7) & ~uintptr_t(7);
                        for (uintptr_t v = vfirst; v + 16 <= vs.addr + vs.size; v += 8) {
                            uint64_t q;
                            std::memcpy(&q, reinterpret_cast<const void*>(v), 8);
                            if (q == uint64_t(col)) return v + 8;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

std::vector<Span> ImageDataSpans(uintptr_t base) {
    // Plain offsets (no <windows.h>), so the tests can use it on a file
    // mapped by hand: e_lfanew at 0x3C; "PE\0\0"; file header (sections at +2,
    // optional header size at +16); 40-byte section headers.
    std::vector<Span> out;
    auto u16 = [](uintptr_t a) {
        uint16_t v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), 2);
        return v;
    };
    if (u16(base) != 0x5A4D) return out; // "MZ"
    const uintptr_t nt = base + U32(base + 0x3C);
    if (U32(nt) != 0x00004550) return out; // "PE\0\0"
    const uint16_t count = u16(nt + 4 + 2);
    const uint16_t optSize = u16(nt + 4 + 16);
    uintptr_t sec = nt + 4 + 20 + optSize;
    for (uint16_t i = 0; i < count; ++i, sec += 40) {
        char name[9] = {};
        std::memcpy(name, reinterpret_cast<const void*>(sec), 8);
        const uint32_t vsize = U32(sec + 8), va = U32(sec + 12), ch = U32(sec + 36);
        const uint32_t kExecute = 0x20000000, kRead = 0x40000000;
        if ((ch & kExecute) || !(ch & kRead) || vsize == 0) continue;
        if (std::strcmp(name, ".reloc") == 0 || std::strcmp(name, ".rsrc") == 0 || std::strcmp(name, ".pdata") == 0)
            continue;
        out.push_back({base + va, size_t(vsize)});
    }
    return out;
}

} // namespace rtti
} // namespace sm2m
