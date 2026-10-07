#include "rom.h"

#include "sha1.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace sm2m {

// Hashes published by the n64decomp/sm64 project (sm64.<ver>.sha1).
const char* const kSm64UsSha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce";
static const char* const kSm64JpSha1 = "8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51";
static const char* const kSm64EuSha1 = "4ac5721683d0e0b6bbb561b58a71740845dceea9";
static const char* const kSm64ShSha1 = "3f319ae697533a255a1003d09202379d78d5a2e0";

RomOrder DetectRomOrder(const uint8_t* d, size_t size) {
    if (size < 4) return RomOrder::Unknown;
    if (d[0] == 0x80 && d[1] == 0x37 && d[2] == 0x12 && d[3] == 0x40) return RomOrder::Z64;
    if (d[0] == 0x37 && d[1] == 0x80 && d[2] == 0x40 && d[3] == 0x12) return RomOrder::V64;
    if (d[0] == 0x40 && d[1] == 0x12 && d[2] == 0x37 && d[3] == 0x80) return RomOrder::N64;
    return RomOrder::Unknown;
}

const char* RomOrderName(RomOrder order) {
    switch (order) {
    case RomOrder::Z64: return ".z64 (big-endian)";
    case RomOrder::V64: return ".v64 (byte-swapped)";
    case RomOrder::N64: return ".n64 (little-endian)";
    default: return "unknown";
    }
}

void NormalizeRomToZ64(std::vector<uint8_t>& rom, RomOrder order) {
    const size_t n = rom.size();
    if (order == RomOrder::V64) {
        for (size_t i = 0; i + 1 < n; i += 2) std::swap(rom[i], rom[i + 1]);
    } else if (order == RomOrder::N64) {
        for (size_t i = 0; i + 3 < n; i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }
}

RomCheck CheckRom(std::vector<uint8_t>& rom) {
    RomCheck r;
    r.order = DetectRomOrder(rom.data(), rom.size());
    if (r.order == RomOrder::Unknown) {
        r.version = "unknown";
        r.message = "not an N64 ROM (unrecognised header)";
        return r;
    }
    NormalizeRomToZ64(rom, r.order);
    r.sha1 = Sha1::HexOf(rom.data(), rom.size());
    if (r.sha1 == kSm64UsSha1) {
        r.ok = true;
        r.version = "US";
        r.message = "Super Mario 64 (US) - OK";
    } else if (r.sha1 == kSm64JpSha1) {
        r.version = "JP";
        r.message = "this is the Japanese ROM; libsm64 needs the US ROM";
    } else if (r.sha1 == kSm64EuSha1) {
        r.version = "EU";
        r.message = "this is the European ROM; libsm64 needs the US ROM";
    } else if (r.sha1 == kSm64ShSha1) {
        r.version = "Shindou";
        r.message = "this is the Shindou ROM; libsm64 needs the US ROM";
    } else {
        r.version = "unknown";
        r.message = "unrecognised ROM (modified or bad dump); libsm64 needs the unmodified US ROM";
    }
    return r;
}

static bool ReadFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamoff size = f.tellg();
    // SM64 is 8 MiB; refuse anything absurd.
    if (size <= 0 || size > 64ll * 1024 * 1024) return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return bool(f);
}

bool FindAndLoadRom(const std::filesystem::path& dir, const std::string& preferredName,
                    std::vector<uint8_t>& out, RomCheck& result, std::filesystem::path& foundPath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    std::error_code ec;
    if (!preferredName.empty()) {
        fs::path p = dir / fs::u8path(preferredName);
        if (fs::is_regular_file(p, ec)) candidates.push_back(p);
    }
    if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string ext = e.path().extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".z64" || ext == ".n64" || ext == ".v64" || ext == ".rom" || ext == ".bin") {
                if (std::find(candidates.begin(), candidates.end(), e.path()) == candidates.end())
                    candidates.push_back(e.path());
            }
        }
    }

    result = RomCheck{};
    result.message = "no ROM file found";
    for (const auto& c : candidates) {
        std::vector<uint8_t> data;
        if (!ReadFile(c, data)) continue;
        RomCheck check = CheckRom(data);
        result = check;
        foundPath = c;
        if (check.ok) {
            out.swap(data);
            return true;
        }
    }
    return false;
}

} // namespace sm2m
