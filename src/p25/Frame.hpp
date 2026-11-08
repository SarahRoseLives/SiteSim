#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>

namespace p25 {

constexpr uint64_t kFrameSync         = 0x5575F5FF77FFull;
// P25 Phase 1 outbound (OSP) status symbols, one interstitial dibit per
// 7.5 ms microslot:
//   0b01 = busy (inbound control channel not available)
//   0b10 = between slot boundaries
//   0b11 = idle (marks the START of an inbound slot)
//   0b00 = not used
// The inbound control channel uses slotted ALOHA: the FIRST status symbol of
// every OSP (a 37.5 ms / 5-microslot frame) must be 0b11 so subscribers can
// synchronize and fit a 32.92 ms ISP, and the remaining four microslots are
// 0b10 "between slot boundaries". A constant value never presents a valid slot.
constexpr uint8_t  kTSCCStatusSlotStart = 0x03;  // 0b11 = idle, start of inbound slot
constexpr uint8_t  kTSCCStatusBetween   = 0x02;  // 0b10 = between slot boundaries
constexpr uint8_t  kTSCCStatusSymbol    = kTSCCStatusSlotStart;  // first symbol of frame

// Convert bytes to dibits (MSB first, 4 dibits per byte)
void bytesToDibits(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

// Extract n dibits from uint64, MSB-first
void uint64ToDibits(uint64_t v, int n, std::vector<uint8_t>& out);

// Insert P25 status symbols: one SS after every 35 data dibits (counter from 1)
std::vector<uint8_t> insertStatusSymbols(const std::vector<uint8_t>& data, uint8_t ss);

// Assemble a complete P25 TSBK frame as a dibit slice
std::vector<uint8_t> BuildFrame(uint16_t nac, const std::array<uint8_t, 12>& tsbk);

// Assemble a P25 TDU frame as a dibit slice
std::vector<uint8_t> BuildTDUFrame(uint16_t nac);

} // namespace p25
