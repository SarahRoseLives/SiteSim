#pragma once
#include <cstdint>

namespace p25 {

constexpr uint8_t DUID_TDU  = 0x03;
constexpr uint8_t DUID_TSBK = 0x07;

// BCH(64,16,23) generator matrix from TIA-102.BAAA-A
static constexpr uint64_t kBchMatrix[16] = {
    0x8000cd930bdd3b2a, 0x4000ab5a8e33a6be,
    0x2000983e4cc4e874, 0x10004c1f2662743a,
    0x0800eb9c98ec0136, 0x0400b85d47ab3bb0,
    0x02005c2ea3d59dd8, 0x01002e1751eaceec,
    0x0080170ba8f56776, 0x0040c616dfa78890,
    0x0020630b6fd3c448, 0x00103185b7e9e224,
    0x000818c2dbf4f112, 0x0004c1f2662743a2,
    0x0002ad6a38ce9afb, 0x00019b2617ba7657,
};

// Encode 16-bit NID data (NAC<<4 | DUID) into 64-bit BCH codeword
inline uint64_t encodeBCH(uint16_t data) {
    uint64_t codeword = 0;
    for (int i = 0; i < 16; i++) {
        if (data & (0x8000u >> i))
            codeword ^= kBchMatrix[i];
    }
    return codeword;
}

} // namespace p25
