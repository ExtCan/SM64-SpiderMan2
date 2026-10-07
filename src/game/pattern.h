// Byte-pattern scanning. Syntax matches LDD565's SM2ScriptTemplate so
// community signatures can be pasted into bindings.ini unchanged:
//   "48 8B 05 ** ** ** ** 48 85 C0 74 ??"
//   hex byte = must match, ?? = wildcard, ** = wildcard that marks the
//   rip-relative displacement / call target to resolve.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sm2m {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask; // 1 = compare, 0 = wildcard
    int captureOffset = -1;    // index of first "**" byte, -1 if none
    bool valid = false;

    static Pattern Parse(const std::string& text);
    size_t size() const { return bytes.size(); }
};

// Returns the offset of the first match in [data, data+size), or -1.
// `startAt` lets callers find subsequent matches.
int64_t FindPattern(const uint8_t* data, size_t size, const Pattern& p, size_t startAt = 0);
// Counts matches (stops at `limit`), useful to report ambiguous signatures.
int CountPattern(const uint8_t* data, size_t size, const Pattern& p, int limit = 8);

enum class Resolve { Direct, Call, Rip };
bool ParseResolve(const std::string& s, Resolve& out);

// Resolves a match at `matchAddr` (absolute address). For Call/Rip the
// displacement is read from matchAddr + captureOffset and the instruction is
// assumed to end right after it (true for E8 rel32, 48 8D 0D disp32,
// 48 89 05 disp32, 48 8B 05 disp32, ...). `extraOffset` is added last.
uintptr_t ResolveMatch(uintptr_t matchAddr, const Pattern& p, Resolve mode, int64_t extraOffset);

// Target of a rip-relative disp32 located at `dispAddr` for an instruction
// ending at dispAddr + 4 + trailingBytes.
uintptr_t RipTarget(uintptr_t dispAddr, int trailingBytes = 0);

// Length of [p, p+len) if it consists only of common x64 function-prologue
// instructions (push r64, sub rsp imm, mov [rsp+d8] r64, mov rax rsp,
// mov [rax+d8] r64, mov rbp rsp), else 0.
int PrologueLength(const uint8_t* p, int len);

// Checks that a signature resolved to the start of a function, as compiled by
// MSVC/clang for x64 (functions are 16-byte aligned, padded with int3).
//   before16: the 16 bytes preceding `addr` (before16[15] is addr - 1)
// Returns addr if it is a plausible start; the earlier aligned start when the
// signature matched a few bytes into a function and only prologue
// instructions separate the two (the game added e.g. a `push rbx`); or 0 when
// addr is in the middle of some function and must not be called.
uintptr_t ValidateFunctionStart(uintptr_t addr, const uint8_t before16[16]);

} // namespace sm2m
