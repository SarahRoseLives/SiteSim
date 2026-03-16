#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>

namespace p25 {

constexpr uint64_t kFrameSync         = 0x5575F5FF77FFull;
constexpr uint8_t  kTSCCStatusSymbol  = 0x02;

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
