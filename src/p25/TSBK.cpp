#include "TSBK.hpp"
#include <cstdint>
#include <array>

namespace p25 {

// Trellis table: [state][inputDibit] = {outDibit0, outDibit1}
static const uint8_t kTrellisTable[4][4][2] = {
    {{0,2},{3,0},{0,1},{3,3}},
    {{3,2},{0,0},{3,1},{0,3}},
    {{2,1},{1,3},{2,2},{1,0}},
    {{1,1},{2,3},{1,2},{2,0}},
};

uint16_t crcCCITT(uint16_t high, uint64_t low) {
    const uint32_t poly = 0x1021;
    uint32_t crc = 0;
    for (int i = 15; i >= 0; i--) {
        crc <<= 1;
        uint32_t bit = (high >> i) & 1u;
        if (((crc >> 16) ^ bit) & 1u) crc ^= poly;
    }
    for (int i = 63; i >= 0; i--) {
        crc <<= 1;
        uint32_t bit = (uint32_t)((low >> i) & 1u);
        if (((crc >> 16) ^ bit) & 1u) crc ^= poly;
    }
    return static_cast<uint16_t>((crc & 0xffff) ^ 0xffff);
}

std::array<uint8_t, 98> trellisEncode(const std::array<uint8_t, 48>& input) {
    // 48 input dibits + 1 flush = 49 inputs → 98 output dibits
    std::array<uint8_t, 98> output{};
    int state = 0;
    int outIdx = 0;
    for (int n = 0; n < 49; n++) {
        uint8_t d = (n < 48) ? input[n] : 0;
        const uint8_t* pair = kTrellisTable[state][d];
        output[outIdx++] = pair[0];
        output[outIdx++] = pair[1];
        state = d;
    }
    return output;
}

std::array<uint8_t, 98> dataInterleave(const std::array<uint8_t, 98>& input) {
    std::array<uint8_t, 98> output{};
    int outIdx = 0;

    // Port of DataInterleave from trellis.go: j+=8 loop pattern
    for (int j = 0; j < 97; j += 8) {
        output[outIdx++] = input[j];
        output[outIdx++] = input[j + 1];
    }
    for (int i = 2; i < 7; i += 2) {
        for (int j = 0; j < 89; j += 8) {
            output[outIdx++] = input[i + j];
            output[outIdx++] = input[i + j + 1];
        }
    }
    return output;
}

std::array<uint8_t, 12> buildTSBK(bool lastBlock, uint8_t opcode, uint8_t mfid, uint64_t args) {
    uint16_t lb = lastBlock ? 1u : 0u;
    uint16_t high = (lb << 15) | (0u << 14) | (uint16_t(opcode) << 8) | uint16_t(mfid);
    uint16_t crc  = crcCCITT(high, args);

    std::array<uint8_t, 12> out{};
    out[0]  = uint8_t(high >> 8);
    out[1]  = uint8_t(high & 0xff);
    for (int i = 0; i < 8; i++)
        out[2 + i] = uint8_t(args >> (56 - 8 * i));
    out[10] = uint8_t(crc >> 8);
    out[11] = uint8_t(crc & 0xff);
    return out;
}

std::array<uint8_t, 12> BuildNetStatusBcast(uint32_t wacn, uint16_t sysID,
                                             uint16_t chanID, uint16_t chanNum, uint8_t ssc) {
    uint64_t ch = (uint64_t(chanID & 0xF) << 12) | uint64_t(chanNum & 0xFFF);
    uint64_t args = (uint64_t(wacn & 0xFFFFF) << 36) |
                    (uint64_t(sysID & 0xFFF) << 24) |
                    (ch << 8) |
                    uint64_t(ssc);
    return buildTSBK(true, TSBK_NET_STS_BCAST, 0x00, args);
}

std::array<uint8_t, 12> BuildRFSSStatusBcast(uint8_t lrar, uint16_t sysID,
                                              uint8_t rfssID, uint8_t siteID,
                                              uint16_t chanID, uint16_t chanNum, uint8_t ssc) {
    uint64_t ch = (uint64_t(chanID & 0xF) << 12) | uint64_t(chanNum & 0xFFF);
    uint64_t args = (uint64_t(lrar) << 56) |
                    (uint64_t(sysID & 0xFFF) << 40) |
                    (uint64_t(rfssID) << 32) |
                    (uint64_t(siteID) << 24) |
                    (ch << 8) |
                    uint64_t(ssc);
    return buildTSBK(true, TSBK_RFSS_STS_BCAST, 0x00, args);
}

std::array<uint8_t, 12> BuildIDENUp(uint8_t iden, uint16_t bwUnits, int16_t offsetUnits,
                                     uint16_t chspacUnits, uint64_t baseFreqHz) {
    uint64_t baseUnits = baseFreqHz / 5;
    uint64_t offsetField = 0;
    if (offsetUnits > 0)
        offsetField = 0x100u | uint64_t(offsetUnits & 0xFF);
    else if (offsetUnits < 0)
        offsetField = uint64_t(-offsetUnits) & 0xFF;

    uint64_t args = (uint64_t(iden & 0xF) << 60) |
                    (uint64_t(bwUnits & 0x1FF) << 51) |
                    (offsetField << 42) |
                    (uint64_t(chspacUnits & 0x3FF) << 32) |
                    (baseUnits & 0xFFFFFFFFull);
    return buildTSBK(true, TSBK_IDEN_UP, 0x00, args);
}

std::array<uint8_t, 12> BuildGrpVChGrant(uint8_t opts, uint16_t chanID, uint16_t chanNum,
                                          uint16_t talkgroup, uint32_t source) {
    uint64_t ch = (uint64_t(chanID & 0xF) << 12) | uint64_t(chanNum & 0xFFF);
    uint64_t args = (uint64_t(opts) << 56) | (ch << 40) |
                    (uint64_t(talkgroup) << 24) | uint64_t(source & 0xFFFFFF);
    return buildTSBK(true, TSBK_GRP_VCH_GRANT, 0x00, args);
}

std::array<uint8_t, 12> BuildGrpVChGrantUpdt(uint16_t chanID1, uint16_t chanNum1, uint16_t tg1,
                                              uint16_t chanID2, uint16_t chanNum2, uint16_t tg2) {
    uint64_t ch1 = (uint64_t(chanID1 & 0xF) << 12) | uint64_t(chanNum1 & 0xFFF);
    uint64_t ch2 = (uint64_t(chanID2 & 0xF) << 12) | uint64_t(chanNum2 & 0xFFF);
    uint64_t args = (ch1 << 48) | (uint64_t(tg1) << 32) | (ch2 << 16) | uint64_t(tg2);
    return buildTSBK(true, TSBK_GRP_VCH_GRANT_UPD, 0x00, args);
}

std::array<uint8_t, 12> BuildGrpAffRsp(bool lg, uint8_t gav,
                                        uint16_t announceGroup, uint16_t talkgroup,
                                        uint32_t source) {
    uint64_t lgBit = lg ? 1ull : 0ull;
    uint64_t args = (lgBit << 63) | (uint64_t(gav & 3) << 56) |
                    (uint64_t(announceGroup) << 40) |
                    (uint64_t(talkgroup) << 24) |
                    uint64_t(source & 0xFFFFFF);
    return buildTSBK(true, TSBK_GRP_AFF_RSP, 0x00, args);
}

std::array<uint8_t, 12> BuildLocRegRsp(uint8_t rv, uint16_t talkgroup,
                                        uint8_t rfssID, uint8_t siteID, uint32_t target) {
    uint64_t args = (uint64_t(rv & 3) << 56) |
                    (uint64_t(talkgroup) << 40) |
                    (uint64_t(rfssID) << 32) |
                    (uint64_t(siteID) << 24) |
                    uint64_t(target & 0xFFFFFF);
    return buildTSBK(true, TSBK_LOC_REG_RSP, 0x00, args);
}

std::array<uint8_t, 12> BuildURegRsp(bool accepted, uint16_t sysID,
                                      uint32_t target, uint32_t source) {
    uint64_t rv = accepted ? 1ull : 0ull;
    uint64_t args = (rv << 60) |
                    (uint64_t(sysID & 0xFFF) << 48) |
                    (uint64_t(target & 0xFFFFFF) << 24) |
                    uint64_t(source & 0xFFFFFF);
    return buildTSBK(true, TSBK_U_REG_RSP, 0x00, args);
}

std::array<uint8_t, 12> BuildAdjStsBcast(uint16_t adjSysID, uint8_t rfssID, uint8_t siteID,
                                          uint16_t chanID, uint16_t chanNum, uint8_t ssc) {
    uint64_t ch = (uint64_t(chanID & 0xF) << 12) | uint64_t(chanNum & 0xFFF);
    uint64_t args = (uint64_t(adjSysID & 0xFFF) << 40) |
                    (uint64_t(rfssID) << 32) |
                    (uint64_t(siteID) << 24) |
                    (ch << 8) |
                    uint64_t(ssc);
    return buildTSBK(true, TSBK_ADJ_STS_BCAST, 0x00, args);
}

} // namespace p25
