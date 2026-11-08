#include "TSBK.hpp"
#include <cstdint>
#include <array>
#include <ctime>

namespace {
// Portable wrappers around the POSIX gmtime_r / localtime_r.  On Windows the
// CRT provides gmtime_s / localtime_s with a different argument order.
inline void gmtimePortable(const std::time_t* t, std::tm* out) {
#if defined(_WIN32)
    gmtime_s(out, t);
#else
    gmtime_r(t, out);
#endif
}

inline void localtimePortable(const std::time_t* t, std::tm* out) {
#if defined(_WIN32)
    localtime_s(out, t);
#else
    localtime_r(t, out);
#endif
}
} // namespace

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
    // Byte[3] high nibble (args bits 55..52) is the CFVA flags field:
    //   bit4 (0x10) = valid RFSS connection active  (args bit 52)
    //   bit5 (0x20) = up to date / correct          (args bit 53)
    // A real ASTRO 25 site sets both (0x3). Setting only the "active" bit marks
    // the site "Last Known" and can make a subscriber reject the control
    // channel, so set both.
    const uint64_t networkActive = (1ull << 52) | (1ull << 53);
    uint64_t args = (uint64_t(lrar) << 56) |
                    (uint64_t(sysID & 0xFFF) << 40) |
                    networkActive |
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
    // TIA-102.AABC Group Voice Channel Grant (opcode 0x00):
    //   octet 2..3 = CHAN, octet 4 = SERVICE OPTIONS, octet 5..6 = GROUP ADDRESS,
    //   octet 7..9 = SOURCE ADDRESS, octet 10..11 = CRC. The channel MUST be at
    //   [2..3]; placing options first shifted CHAN to [3..4] and made the radio
    //   read an invalid identifier (iden 0), so it rejected the grant and kept
    //   re-requesting the voice channel.
    uint64_t args = (uint64_t(ch) << 48) | (uint64_t(opts) << 40) |
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

std::array<uint8_t, 12> BuildSecCcBcast(uint8_t rfssID, uint8_t siteID,
                                        uint16_t chanID1, uint16_t chanNum1, uint8_t ssc1,
                                        uint16_t chanID2, uint16_t chanNum2, uint8_t ssc2) {
    uint64_t ch1 = (uint64_t(chanID1 & 0xF) << 12) | uint64_t(chanNum1 & 0xFFF);
    uint64_t ch2 = (uint64_t(chanID2 & 0xF) << 12) | uint64_t(chanNum2 & 0xFFF);
    // Payload bytes [2..9]: RFSS, Site, CHAN1(16), SSC1, CHAN2(16), SSC2.
    uint64_t args = (uint64_t(rfssID) << 56) |
                    (uint64_t(siteID) << 48) |
                    (ch1 << 32) |
                    (uint64_t(ssc1) << 24) |
                    (ch2 << 8) |
                    uint64_t(ssc2);
    return buildTSBK(true, TSBK_SEC_CC_BCAST, 0x00, args);
}

std::array<uint8_t, 12> BuildSyncBcast() {
    const std::time_t now = std::time(nullptr);

    std::tm utcTm{};
    gmtimePortable(&now, &utcTm);

    // TIA-102.AABC SYNC_BCAST payload (message bits 16..79 = args bits 63..0):
    //   29      system time not locked to external reference
    //   30      micro-slot/minute rollover unlocked
    //   31-32   leap second correction
    //   33      local time offset valid (0 = valid)
    //   34      local time offset sign
    //   35-38   local time offset hours
    //   39      local time offset half hour
    //   40-46   year - 2000
    //   47-50   month (1-12)
    //   51-55   day
    //   56-60   hours
    //   61-66   minutes
    //   67-79   micro-slots since minute rollover (7.5 ms units)
    uint64_t args = 0;
    auto put = [&args](int msbMsg, int lsbMsg, uint64_t value) {
        const int shift = 79 - lsbMsg;
        const int nbits = lsbMsg - msbMsg + 1;
        const uint64_t mask = (nbits >= 64) ? ~0ull : ((1ull << nbits) - 1ull);
        args |= (value & mask) << shift;
    };

    const uint64_t year     = uint64_t((utcTm.tm_year + 1900 - 2000) & 0x7F);
    const uint64_t month    = uint64_t(utcTm.tm_mon + 1) & 0x0F;
    const uint64_t day      = uint64_t(utcTm.tm_mday) & 0x1F;
    const uint64_t hours    = uint64_t(utcTm.tm_hour) & 0x1F;
    const uint64_t minutes  = uint64_t(utcTm.tm_min) & 0x3F;
    uint64_t microSlots     = uint64_t((utcTm.tm_sec * 8000) / 60);
    if (microSlots > 7999) microSlots = 7999;

    put(29, 29, 0);          // locked to external reference
    put(30, 30, 0);          // micro-slot/minute rollover locked
    put(31, 32, 0);          // no leap second correction
    put(33, 33, 0);          // local time offset valid (0 = valid)
    put(34, 34, 0);          // offset sign: positive
    put(35, 38, 0);          // offset hours: 0 (UTC)
    put(39, 39, 0);          // offset half hour: 0
    put(40, 46, year);       // 7 bits
    put(47, 50, month);      // 4 bits
    put(51, 55, day);        // 5 bits
    put(56, 60, hours);      // 5 bits
    put(61, 66, minutes);    // 6 bits
    put(67, 79, microSlots); // 13 bits

    return buildTSBK(true, TSBK_SYNC_BCAST, 0x00, args);
}

// Motorola-proprietary station messages (MFID 0x90).  Argument byte layout used
// by these opcodes: args bits [63:16] -> payload bytes [2..7], args [15:0] ->
// bytes [8..9] (buildTSBK stores args MSB-first into bytes 2..9).
std::array<uint8_t, 12> BuildMotoBaseStationId(uint8_t iden, uint16_t chanNum,
                                               const char* cwid) {
    // CWID: eight 6-bit fields packed MSB-first into 48 bits. Field i occupies
    // bits (42 - 6*i)..(37 - 6*i) of the value carried in bytes[2..7].
    uint64_t cwidBits = 0;
    if (cwid) {
        int n = 0;
        for (const char* p = cwid; *p && n < 8; ++p, ++n) {
            uint64_t v = uint64_t(uint8_t(uint8_t(*p) - 43u) & 0x3F);
            cwidBits |= v << (42 - 6 * n);
        }
    }
    // 16-bit channel field: 4-bit IDEN in the high nibble, 12-bit number low.
    uint64_t channel = (uint64_t(iden & 0xF) << 12) | uint64_t(chanNum & 0xFFF);
    uint64_t args = (cwidBits << 16) | channel;
    return buildTSBK(true, TSBK_MOTO_BASE_STATION_ID, TSBK_MFID_MOTOROLA, args);
}

std::array<uint8_t, 12> BuildMotoTrafficChannelId(uint64_t msg) {
    return buildTSBK(true, TSBK_MOTO_TRAFFIC_CHAN_ID, TSBK_MFID_MOTOROLA, msg);
}

std::array<uint8_t, 12> BuildMotoScanMarker(uint8_t mk, uint8_t ms, uint16_t value) {
    // byte[2] high nibble = marker key, byte[3] = marker segment,
    // bytes[4..5] = 16-bit value, bytes[6..9] zero.
    uint64_t byte2 = uint64_t((mk & 0x0F) << 4);
    uint64_t byte3 = uint64_t(ms & 0xFF);
    uint64_t args = (byte2 << 56) | (byte3 << 48) | (uint64_t(value) << 32);
    return buildTSBK(true, TSBK_MOTO_SCAN_MARKER, TSBK_MFID_MOTOROLA, args);
}

} // namespace p25
