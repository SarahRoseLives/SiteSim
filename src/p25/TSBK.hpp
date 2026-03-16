#pragma once
#include <cstdint>
#include <array>

namespace p25 {

// TSBK opcode constants (outbound, standard MFID 0x00)
constexpr uint8_t TSBK_GRP_VCH_GRANT     = 0x00;
constexpr uint8_t TSBK_GRP_VCH_GRANT_UPD = 0x02;
constexpr uint8_t TSBK_GRP_AFF_RSP       = 0x28;
constexpr uint8_t TSBK_LOC_REG_RSP       = 0x2B;
constexpr uint8_t TSBK_U_REG_RSP         = 0x2C;
constexpr uint8_t TSBK_RFSS_STS_BCAST    = 0x3A;
constexpr uint8_t TSBK_NET_STS_BCAST     = 0x3B;
constexpr uint8_t TSBK_ADJ_STS_BCAST     = 0x3C;
constexpr uint8_t TSBK_IDEN_UP           = 0x3D;

// CRC-CCITT over 80 data bits (16 + 64). Poly=0x1021, init=0, finalXOR=0xFFFF.
uint16_t crcCCITT(uint16_t high, uint64_t low);

// Trellis-encode 48 input dibits → 98 output dibits (flush dibit appended)
std::array<uint8_t, 98> trellisEncode(const std::array<uint8_t, 48>& input);

// P25 data interleave of 98 dibits
std::array<uint8_t, 98> dataInterleave(const std::array<uint8_t, 98>& input);

// Low-level builder: assembles 96-bit TSBK PDU into [12]byte with CRC
std::array<uint8_t, 12> buildTSBK(bool lastBlock, uint8_t opcode, uint8_t mfid, uint64_t args);

// TSBK PDU builders
std::array<uint8_t, 12> BuildNetStatusBcast(uint32_t wacn, uint16_t sysID,
                                             uint16_t chanID, uint16_t chanNum, uint8_t ssc);

std::array<uint8_t, 12> BuildRFSSStatusBcast(uint8_t lrar, uint16_t sysID,
                                              uint8_t rfssID, uint8_t siteID,
                                              uint16_t chanID, uint16_t chanNum, uint8_t ssc);

std::array<uint8_t, 12> BuildIDENUp(uint8_t iden, uint16_t bwUnits, int16_t offsetUnits,
                                     uint16_t chspacUnits, uint64_t baseFreqHz);

std::array<uint8_t, 12> BuildGrpVChGrant(uint8_t opts, uint16_t chanID, uint16_t chanNum,
                                          uint16_t talkgroup, uint32_t source);

std::array<uint8_t, 12> BuildGrpVChGrantUpdt(uint16_t chanID1, uint16_t chanNum1, uint16_t tg1,
                                              uint16_t chanID2, uint16_t chanNum2, uint16_t tg2);

std::array<uint8_t, 12> BuildGrpAffRsp(bool lg, uint8_t gav,
                                        uint16_t announceGroup, uint16_t talkgroup,
                                        uint32_t source);

std::array<uint8_t, 12> BuildLocRegRsp(uint8_t rv, uint16_t talkgroup,
                                        uint8_t rfssID, uint8_t siteID, uint32_t target);

std::array<uint8_t, 12> BuildURegRsp(bool accepted, uint16_t sysID,
                                      uint32_t target, uint32_t source);

std::array<uint8_t, 12> BuildAdjStsBcast(uint16_t adjSysID, uint8_t rfssID, uint8_t siteID,
                                          uint16_t chanID, uint16_t chanNum, uint8_t ssc);

} // namespace p25
