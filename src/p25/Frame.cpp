#include "Frame.hpp"
#include "NID.hpp"
#include "TSBK.hpp"
#include <cstring>

namespace p25 {

void bytesToDibits(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        out.push_back((b >> 6) & 0x03);
        out.push_back((b >> 4) & 0x03);
        out.push_back((b >> 2) & 0x03);
        out.push_back((b >> 0) & 0x03);
    }
}

void uint64ToDibits(uint64_t v, int n, std::vector<uint8_t>& out) {
    // Extract n dibits from uint64, MSB-first (right-aligned)
    // v has 64 bits = 32 dibits; we want the last n dibits
    uint8_t buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = uint8_t(v >> (56 - 8 * i));
    std::vector<uint8_t> all;
    all.reserve(32);
    bytesToDibits(buf, 8, all);
    // append last n elements
    int start = 32 - n;
    for (int i = start; i < 32; i++)
        out.push_back(all[i]);
}

std::vector<uint8_t> insertStatusSymbols(const std::vector<uint8_t>& data, uint8_t ss) {
    int numSS = (int(data.size()) + 34) / 35;
    std::vector<uint8_t> out;
    out.reserve(data.size() + numSS + 35);

    int remaining = numSS;
    int i = 1;
    for (uint8_t d : data) {
        out.push_back(d);
        if (i % 35 == 0 && remaining > 0) {
            out.push_back(ss);
            remaining--;
        }
        i++;
    }
    // Pad with zero dibits until all status symbols have been emitted
    while (remaining > 0) {
        out.push_back(0);
        if (i % 35 == 0) {
            out.push_back(ss);
            remaining--;
        }
        i++;
    }
    return out;
}

std::vector<uint8_t> BuildFrame(uint16_t nac, const std::array<uint8_t, 12>& tsbk) {
    std::vector<uint8_t> frame;
    frame.reserve(154);

    // Frame sync: 24 dibits from 48-bit constant
    uint64ToDibits(kFrameSync, 24, frame);

    // NID: BCH(64,16,23) encode of (NAC<<4 | DUID_TSBK)
    uint64_t nid = encodeBCH((uint16_t(nac) << 4) | DUID_TSBK);
    uint64ToDibits(nid, 32, frame);

    // TSBK payload: 12 bytes → 48 dibits → trellis encode → interleave
    std::array<uint8_t, 48> inputDibits{};
    {
        std::vector<uint8_t> tmp;
        tmp.reserve(48);
        bytesToDibits(tsbk.data(), 12, tmp);
        for (int i = 0; i < 48; i++) inputDibits[i] = tmp[i];
    }
    auto encoded    = trellisEncode(inputDibits);
    auto interleaved = dataInterleave(encoded);

    for (auto d : interleaved)
        frame.push_back(d);

    return insertStatusSymbols(frame, kTSCCStatusSymbol);
}

std::vector<uint8_t> BuildTDUFrame(uint16_t nac) {
    std::vector<uint8_t> frame;
    frame.reserve(154);

    uint64ToDibits(kFrameSync, 24, frame);

    uint64_t nid = encodeBCH((uint16_t(nac) << 4) | DUID_TDU);
    uint64ToDibits(nid, 32, frame);

    for (int i = 0; i < 98; i++)
        frame.push_back(0);

    return insertStatusSymbols(frame, kTSCCStatusSymbol);
}

} // namespace p25
