#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sm2m {

// Plain SHA-1 (FIPS 180-4). Only used to identify the user's SM64 ROM.
class Sha1 {
public:
    Sha1();
    void Update(const void* data, size_t len);
    std::array<uint8_t, 20> Final();

    static std::string Hex(const std::array<uint8_t, 20>& digest);
    static std::string HexOf(const void* data, size_t len);

private:
    void Block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t totalLen_ = 0;
};

} // namespace sm2m
