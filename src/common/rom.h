// SM64 ROM discovery and validation. libsm64 needs the US ROM in big-endian
// (.z64) byte order; .v64 and .n64 dumps are converted in memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sm2m {

enum class RomOrder { Z64, V64, N64, Unknown };

struct RomCheck {
    bool ok = false;           // true only for the US ROM
    RomOrder order = RomOrder::Unknown;
    std::string sha1;          // of the normalised (.z64) image
    std::string version;       // "US", "JP", "EU", "Shindou" or "unknown"
    std::string message;       // human readable result
};

extern const char* const kSm64UsSha1;

RomOrder DetectRomOrder(const uint8_t* data, size_t size);
const char* RomOrderName(RomOrder order);
// Converts `rom` to .z64 byte order in place.
void NormalizeRomToZ64(std::vector<uint8_t>& rom, RomOrder order);
// Normalises in place and identifies the ROM.
RomCheck CheckRom(std::vector<uint8_t>& rom);

// Looks for a usable ROM in `dir` (preferring `preferredName`), loads it into
// `out` (normalised) and reports what it found. Returns true if a US ROM was
// loaded.
bool FindAndLoadRom(const std::filesystem::path& dir, const std::string& preferredName,
                    std::vector<uint8_t>& out, RomCheck& result, std::filesystem::path& foundPath);

} // namespace sm2m
