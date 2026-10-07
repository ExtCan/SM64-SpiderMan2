#include "pattern.h"

#include <cctype>
#include <cstring>

#include "../common/ini.h"

namespace sm2m {

Pattern Pattern::Parse(const std::string& text) {
    Pattern p;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= n) break;
        if (i + 1 >= n) return Pattern{};
        char a = text[i], b = text[i + 1];
        if ((a == '?' && b == '?') || (a == '*' && b == '*')) {
            if (a == '*' && p.captureOffset < 0) p.captureOffset = int(p.bytes.size());
            p.bytes.push_back(0);
            p.mask.push_back(0);
        } else if (a == '?' && (i + 1 >= n || std::isspace(static_cast<unsigned char>(b)))) {
            // Single '?' wildcard.
            p.bytes.push_back(0);
            p.mask.push_back(0);
            i += 1;
            continue;
        } else if (std::isxdigit(static_cast<unsigned char>(a)) && std::isxdigit(static_cast<unsigned char>(b))) {
            char hex[3] = {a, b, 0};
            p.bytes.push_back(uint8_t(std::strtoul(hex, nullptr, 16)));
            p.mask.push_back(1);
        } else {
            return Pattern{};
        }
        i += 2;
    }
    p.valid = !p.bytes.empty();
    // A pattern that is all wildcards would match anywhere.
    bool anyFixed = false;
    for (uint8_t m : p.mask) anyFixed |= (m != 0);
    if (!anyFixed) p.valid = false;
    return p;
}

int64_t FindPattern(const uint8_t* data, size_t size, const Pattern& p, size_t startAt) {
    if (!p.valid || size < p.size()) return -1;
    // Anchor on the first fixed byte to use memchr.
    size_t anchor = 0;
    while (anchor < p.size() && !p.mask[anchor]) ++anchor;
    const uint8_t first = p.bytes[anchor];
    const size_t last = size - p.size();
    size_t pos = startAt;
    while (pos <= last) {
        const void* hit = std::memchr(data + pos + anchor, first, last - pos + 1);
        if (!hit) return -1;
        size_t cand = size_t(static_cast<const uint8_t*>(hit) - data) - anchor;
        bool ok = true;
        for (size_t k = 0; k < p.size(); ++k) {
            if (p.mask[k] && data[cand + k] != p.bytes[k]) {
                ok = false;
                break;
            }
        }
        if (ok) return int64_t(cand);
        pos = cand + 1;
    }
    return -1;
}

int CountPattern(const uint8_t* data, size_t size, const Pattern& p, int limit) {
    int count = 0;
    size_t start = 0;
    while (count < limit) {
        int64_t off = FindPattern(data, size, p, start);
        if (off < 0) break;
        ++count;
        start = size_t(off) + 1;
    }
    return count;
}

bool ParseResolve(const std::string& s, Resolve& out) {
    std::string v = Ini::Lower(Ini::Trim(s));
    if (v.empty() || v == "direct") {
        out = Resolve::Direct;
        return true;
    }
    if (v == "call") {
        out = Resolve::Call;
        return true;
    }
    if (v == "rip" || v == "ref") {
        out = Resolve::Rip;
        return true;
    }
    return false;
}

uintptr_t RipTarget(uintptr_t dispAddr, int trailingBytes) {
    int32_t disp;
    std::memcpy(&disp, reinterpret_cast<const void*>(dispAddr), sizeof(disp));
    return dispAddr + 4 + uintptr_t(intptr_t(trailingBytes)) + uintptr_t(intptr_t(disp));
}

uintptr_t ResolveMatch(uintptr_t matchAddr, const Pattern& p, Resolve mode, int64_t extraOffset) {
    uintptr_t result = matchAddr;
    if (mode == Resolve::Call || mode == Resolve::Rip) {
        int capture = p.captureOffset;
        if (capture < 0) capture = (mode == Resolve::Call) ? 1 : 3;
        result = RipTarget(matchAddr + uintptr_t(capture));
    }
    return result + uintptr_t(intptr_t(extraOffset));
}

int PrologueLength(const uint8_t* p, int len) {
    int i = 0;
    while (i < len) {
        const uint8_t b = p[i];
        const int left = len - i;
        if (b >= 0x50 && b <= 0x57) { // push r64
            i += 1;
        } else if ((b == 0x40 || b == 0x41 || b == 0x48 || b == 0x49) && left >= 2 && p[i + 1] >= 0x50 &&
                   p[i + 1] <= 0x57) { // push with REX (40 53 = push rbx, 41 56 = push r14)
            i += 2;
        } else if (b == 0x48 && left >= 4 && p[i + 1] == 0x83 && p[i + 2] == 0xEC) { // sub rsp, imm8
            i += 4;
        } else if (b == 0x48 && left >= 7 && p[i + 1] == 0x81 && p[i + 2] == 0xEC) { // sub rsp, imm32
            i += 7;
        } else if ((b == 0x48 || b == 0x4C) && left >= 5 && p[i + 1] == 0x89 && (p[i + 2] & 0xC7) == 0x44 &&
                   p[i + 3] == 0x24) { // mov [rsp+disp8], r64
            i += 5;
        } else if (b == 0x48 && left >= 3 && p[i + 1] == 0x8B && p[i + 2] == 0xC4) { // mov rax, rsp
            i += 3;
        } else if ((b == 0x48 || b == 0x4C) && left >= 4 && p[i + 1] == 0x89 && (p[i + 2] & 0xC7) == 0x40) {
            i += 4; // mov [rax+disp8], r64
        } else if (b == 0x48 && left >= 3 && ((p[i + 1] == 0x8B && p[i + 2] == 0xEC) ||
                                             (p[i + 1] == 0x89 && p[i + 2] == 0xE5))) { // mov rbp, rsp
            i += 3;
        } else {
            return 0;
        }
    }
    return i == len ? len : 0;
}

uintptr_t ValidateFunctionStart(uintptr_t addr, const uint8_t before16[16]) {
    if ((addr & 15) == 0) return addr;
    if (before16[15] == 0xCC) return addr; // unaligned, but right after int3 padding
    const int gap = int(addr & 15);
    // The aligned candidate must follow padding or the previous function's ret.
    const uint8_t prev = before16[15 - gap];
    if (prev != 0xCC && prev != 0xC3) return 0;
    if (PrologueLength(before16 + (16 - gap), gap) != gap) return 0;
    return addr - uintptr_t(gap);
}

} // namespace sm2m
