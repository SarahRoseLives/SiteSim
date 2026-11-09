#pragma once
#include <cstdint>
#include <array>

namespace p25 {

// TSBK opcode constants (outbound, standard MFID 0x00)
constexpr uint8_t TSBK_GRP_VCH_GRANT     = 0x00;
constexpr uint8_t TSBK_GRP_VCH_GRANT_UPD = 0x02;
constexpr uint8_t TSBK_GRP_AFF_RSP       = 0x28;
constexpr uint8_t TSBK_GRP_AFF_Q         = 0x2A;  // ask a unit to report affiliation
constexpr uint8_t TSBK_LOC_REG_RSP       = 0x2B;
constexpr uint8_t TSBK_U_REG_RSP         = 0x2C;
constexpr uint8_t TSBK_U_DE_REG_ACK      = 0x2F;  // acknowledge de-registration
constexpr uint8_t TSBK_SYNC_BCAST        = 0x30;
constexpr uint8_t TSBK_SEC_CC_BCAST      = 0x39;
constexpr uint8_t TSBK_RFSS_STS_BCAST    = 0x3A;
constexpr uint8_t TSBK_NET_STS_BCAST     = 0x3B;
constexpr uint8_t TSBK_ADJ_STS_BCAST     = 0x3C;
constexpr uint8_t TSBK_IDEN_UP           = 0x3D;

// Motorola-proprietary outbound TSBKs (manufacturer ID 0x90). A real Motorola
// ASTRO 25 site broadcasts these on the control channel; a Motorola subscriber
// looks for the Control Channel Base Station ID in particular.
constexpr uint8_t TSBK_MFID_MOTOROLA          = 0x90;
constexpr uint8_t TSBK_MOTO_TRAFFIC_CHAN_ID   = 0x05;
constexpr uint8_t TSBK_MOTO_SCAN_MARKER       = 0x09;
constexpr uint8_t TSBK_MOTO_BASE_STATION_ID   = 0x0B;

// System Service Class bits carried in NET_STS/RFSS_STS/ADJ_STS. A value of
// 0x00 means "no services", which makes subscribers treat the site as
// unusable. Advertise the services a voice site actually offers.
constexpr uint8_t SSC_COMPOSITE_CC   = 0x01;
constexpr uint8_t SSC_DATA           = 0x10;
constexpr uint8_t SSC_VOICE          = 0x20;
constexpr uint8_t SSC_REGISTRATION   = 0x40;
constexpr uint8_t SSC_AUTHENTICATION = 0x80;
constexpr uint8_t SSC_VOICE_REGISTRATION = SSC_VOICE | SSC_REGISTRATION;
// A real Motorola ASTRO 25 voice site advertises Data + Voice + Registration
// (0x70) in NET_STS/RFSS_STS/ADJ_STS; match it so a Moto subscriber sees the
// same service class it expects from its own infrastructure.
constexpr uint8_t SSC_VOICE_DATA_REGISTRATION =
    SSC_DATA | SSC_VOICE | SSC_REGISTRATION;

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

// GRP_AFF_Q (0x2A): ask a target unit to report its group affiliation.
// Payload: reserved(16), TARGET_ADDRESS(24), SOURCE_ADDRESS(24).
std::array<uint8_t, 12> BuildGrpAffQuery(uint32_t target, uint32_t source);

std::array<uint8_t, 12> BuildLocRegRsp(uint8_t rv, uint16_t talkgroup,
                                        uint8_t rfssID, uint8_t siteID, uint32_t target);

std::array<uint8_t, 12> BuildURegRsp(bool accepted, uint16_t sysID,
                                      uint32_t target, uint32_t source);

// U_DE_REG_ACK (0x2F): acknowledge a unit de-registration. Payload:
// reserved(8), WACN(20), SYSTEM(12), TARGET_ADDRESS(24).
std::array<uint8_t, 12> BuildUDeRegAck(uint32_t wacn, uint16_t sysID, uint32_t target);

std::array<uint8_t, 12> BuildAdjStsBcast(uint16_t adjSysID, uint8_t rfssID, uint8_t siteID,
                                          uint16_t chanID, uint16_t chanNum, uint8_t ssc);

// SEC_CC_BCAST (0x39): secondary control channel broadcast (implicit form).
// Carries up to two backup control channels, each with its own service class.
std::array<uint8_t, 12> BuildSecCcBcast(uint8_t rfssID, uint8_t siteID,
                                        uint16_t chanID1, uint16_t chanNum1, uint8_t ssc1,
                                        uint16_t chanID2, uint16_t chanNum2, uint8_t ssc2);

// SYNC_BCAST (0x30): broadcasts current UTC date/time and local offset.
std::array<uint8_t, 12> BuildSyncBcast();

// ── Motorola-proprietary (MFID 0x90) station messages ────────────────────
// Control Channel Base Station ID (opcode 0x0B): 8 x 6-bit CWID characters
// (ASCII-43, MSB first, blank when the site has no call sign) followed by a
// 16-bit channel field (4-bit IDEN | 12-bit channel number).
std::array<uint8_t, 12> BuildMotoBaseStationId(uint8_t iden, uint16_t chanNum,
                                               const char* cwid = "");

// Traffic Channel ID (opcode 0x05): 8 opaque payload bytes broadcast verbatim.
std::array<uint8_t, 12> BuildMotoTrafficChannelId(uint64_t msg);

// Scan Marker Broadcast (opcode 0x09): 4-bit marker key, 8-bit marker segment
// and a 16-bit value.
std::array<uint8_t, 12> BuildMotoScanMarker(uint8_t mk, uint8_t ms, uint16_t value);

} // namespace p25
