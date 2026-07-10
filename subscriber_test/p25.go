package main

// P25 Phase 1 encoding stack — ported from SiteSim's C++ implementation.
// Supports building ISP (mobile-originated) TSBKs, assembling them into
// complete P25 frames, and C4FM-modulating to int8 IQ samples for HackRF.

import (
	"math"
)

// ── Constants ─────────────────────────────────────────────────────────────────

const (
	sampleRate  = 2_400_000.0
	symbolRate  = 4_800.0
	osr         = 500 // samples per symbol
	deviationHz = 600.0
	rrcAlpha    = 0.2
	rrcSpan     = 4
	polyTaps    = rrcSpan*2 + 1 // 9 – symbol history depth

	frameSync uint64 = 0x5575F5FF77FF

	duidTSBK uint8 = 0x07

	// ISP TSBK opcodes (mobile-originated, TIA-102.AABC-E)
	ispGrpVchReq   uint8 = 0x00
	ispUuVchReq    uint8 = 0x03
	ispEmrgAlrmReq uint8 = 0x0F
	ispGrpAffReq   uint8 = 0x28
	ispURegReq     uint8 = 0x2C
	ispUDeRegAck   uint8 = 0x2F
)

// Grey-coded dibit → C4FM symbol amplitude multiplier.
// {0,1,2,3} → {+1,+3,-1,-3}  (matches SiteSim's kSymbol table)
var symVal = [4]float64{+1, +3, -1, -3}

// ── BCH NID encoding ──────────────────────────────────────────────────────────

// bchMatrix rows encode the BCH(64,16,23) generator used for the P25 NID.
var bchMatrix = [16]uint64{
	0x8000cd930bdd3b2a, 0x4000ab5a8e33a6be,
	0x2000983e4cc4e874, 0x10004c1f2662743a,
	0x0800eb9c98ec0136, 0x0400b85d47ab3bb0,
	0x02005c2ea3d59dd8, 0x01002e1751eaceec,
	0x0080170ba8f56776, 0x0040c616dfa78890,
	0x0020630b6fd3c448, 0x00103185b7e9e224,
	0x000818c2dbf4f112, 0x0004c1f2662743a2,
	0x0002ad6a38ce9afb, 0x00019b2617ba7657,
}

func encodeBCH(data uint16) uint64 {
	var codeword uint64
	for i := 0; i < 16; i++ {
		if data&(0x8000>>uint(i)) != 0 {
			codeword ^= bchMatrix[i]
		}
	}
	return codeword
}

// ── CRC-CCITT ─────────────────────────────────────────────────────────────────

// crcCCITT computes CRC-CCITT over 80 bits (16-bit high + 64-bit low).
// Poly = 0x1021, init = 0, finalXOR = 0xFFFF.
func crcCCITT(high uint16, low uint64) uint16 {
	const poly = 0x1021
	var crc uint32
	for i := 15; i >= 0; i-- {
		crc <<= 1
		bit := uint32((high >> uint(i)) & 1)
		if ((crc>>16)^bit)&1 != 0 {
			crc ^= poly
		}
	}
	for i := 63; i >= 0; i-- {
		crc <<= 1
		bit := uint32((low >> uint(i)) & 1)
		if ((crc>>16)^bit)&1 != 0 {
			crc ^= poly
		}
	}
	return uint16((crc & 0xffff) ^ 0xffff)
}

// ── Trellis encoder + data interleave ─────────────────────────────────────────

// trellisTable[state][inputDibit] = {outDibit0, outDibit1}
var trellisTable = [4][4][2]uint8{
	{{0, 2}, {3, 0}, {0, 1}, {3, 3}},
	{{3, 2}, {0, 0}, {3, 1}, {0, 3}},
	{{2, 1}, {1, 3}, {2, 2}, {1, 0}},
	{{1, 1}, {2, 3}, {1, 2}, {2, 0}},
}

// trellisEncode converts 48 input dibits to 98 output dibits (rate-1/2 + flush).
func trellisEncode(input [48]uint8) [98]uint8 {
	var output [98]uint8
	state := 0
	outIdx := 0
	for n := 0; n < 49; n++ {
		var d uint8
		if n < 48 {
			d = input[n]
		}
		pair := trellisTable[state][d]
		output[outIdx] = pair[0]
		outIdx++
		output[outIdx] = pair[1]
		outIdx++
		state = int(d)
	}
	return output
}

// dataInterleave applies the P25 dibit interleave pattern to 98 dibits.
func dataInterleave(input [98]uint8) [98]uint8 {
	var output [98]uint8
	outIdx := 0
	for j := 0; j < 97; j += 8 {
		output[outIdx] = input[j]
		outIdx++
		output[outIdx] = input[j+1]
		outIdx++
	}
	for i := 2; i < 7; i += 2 {
		for j := 0; j < 89; j += 8 {
			output[outIdx] = input[i+j]
			outIdx++
			output[outIdx] = input[i+j+1]
			outIdx++
		}
	}
	return output
}

// ── TSBK low-level builder ────────────────────────────────────────────────────

// buildTSBK assembles a 12-byte TSBK PDU with CRC-CCITT.
func buildTSBK(lastBlock bool, opcode, mfid uint8, args uint64) [12]byte {
	var lb uint16
	if lastBlock {
		lb = 1
	}
	high := (lb << 15) | (uint16(opcode) << 8) | uint16(mfid)
	crc := crcCCITT(high, args)

	var out [12]byte
	out[0] = byte(high >> 8)
	out[1] = byte(high & 0xff)
	for i := 0; i < 8; i++ {
		out[2+i] = byte(args >> (56 - 8*uint(i)))
	}
	out[10] = byte(crc >> 8)
	out[11] = byte(crc & 0xff)
	return out
}

// ── ISP TSBK builders (mobile-originated) ────────────────────────────────────

// BuildGrpVchReq builds a Group Voice Channel Request (ISP 0x00).
func BuildGrpVchReq(opts uint8, tgid uint16, srcID uint32) [12]byte {
	args := (uint64(opts) << 56) | (uint64(tgid) << 24) | uint64(srcID&0xFFFFFF)
	return buildTSBK(true, ispGrpVchReq, 0x00, args)
}

// BuildUuVchReq builds a Unit-to-Unit Voice Channel Request (ISP 0x03).
func BuildUuVchReq(opts uint8, targetID, srcID uint32) [12]byte {
	args := (uint64(opts) << 56) | (uint64(targetID&0xFFFFFF) << 24) | uint64(srcID&0xFFFFFF)
	return buildTSBK(true, ispUuVchReq, 0x00, args)
}

// BuildEmrgAlrmReq builds an Emergency Alarm Request (ISP 0x0F).
func BuildEmrgAlrmReq(tgid uint16, srcID uint32) [12]byte {
	args := (uint64(tgid) << 24) | uint64(srcID&0xFFFFFF)
	return buildTSBK(true, ispEmrgAlrmReq, 0x00, args)
}

// BuildGrpAffReq builds a Group Affiliation Request (ISP 0x28).
func BuildGrpAffReq(tgid uint16, srcID uint32) [12]byte {
	args := (uint64(tgid) << 24) | uint64(srcID&0xFFFFFF)
	return buildTSBK(true, ispGrpAffReq, 0x00, args)
}

// BuildURegReq builds a Unit Registration Request (ISP 0x2C).
func BuildURegReq(sysID uint16, srcID uint32) [12]byte {
	args := (uint64(sysID&0xFFF) << 36) | uint64(srcID&0xFFFFFF)
	return buildTSBK(true, ispURegReq, 0x00, args)
}

// BuildUDeRegAck builds a Unit De-Registration Acknowledge (ISP 0x2F).
func BuildUDeRegAck(srcID uint32) [12]byte {
	args := uint64(srcID & 0xFFFFFF)
	return buildTSBK(true, ispUDeRegAck, 0x00, args)
}

// ── P25 frame assembly ────────────────────────────────────────────────────────

func bytesToDibits(data []byte) []uint8 {
	out := make([]uint8, 0, len(data)*4)
	for _, b := range data {
		out = append(out,
			(b>>6)&0x03,
			(b>>4)&0x03,
			(b>>2)&0x03,
			(b>>0)&0x03,
		)
	}
	return out
}

func uint64ToDibits(v uint64, n int) []uint8 {
	var buf [8]byte
	for i := 0; i < 8; i++ {
		buf[i] = byte(v >> (56 - 8*uint(i)))
	}
	all := bytesToDibits(buf[:])
	return all[32-n:]
}

// insertStatusSymbols inserts a P25 status symbol after every 35 data dibits.
func insertStatusSymbols(data []uint8, ss uint8) []uint8 {
	numSS := (len(data) + 34) / 35
	out := make([]uint8, 0, len(data)+numSS+35)

	remaining := numSS
	i := 1
	for _, d := range data {
		out = append(out, d)
		if i%35 == 0 && remaining > 0 {
			out = append(out, ss)
			remaining--
		}
		i++
	}
	for remaining > 0 {
		out = append(out, 0)
		if i%35 == 0 {
			out = append(out, ss)
			remaining--
		}
		i++
	}
	return out
}

// BuildFrame assembles a complete P25 TSBK frame as a dibit slice.
func BuildFrame(nac uint16, tsbk [12]byte) []uint8 {
	frame := make([]uint8, 0, 154)

	// Frame sync: 24 dibits from 48-bit constant
	frame = append(frame, uint64ToDibits(frameSync, 24)...)

	// NID: BCH(64,16,23) encode of (NAC<<4 | DUID_TSBK)
	nid := encodeBCH((nac << 4) | uint16(duidTSBK))
	frame = append(frame, uint64ToDibits(nid, 32)...)

	// TSBK payload: 12 bytes → 48 dibits → trellis → interleave
	rawDibits := bytesToDibits(tsbk[:])
	var inputArr [48]uint8
	copy(inputArr[:], rawDibits[:48])
	encoded := trellisEncode(inputArr)
	interleaved := dataInterleave(encoded)

	for _, d := range interleaved {
		frame = append(frame, d)
	}

	return insertStatusSymbols(frame, 0x02)
}

// ── C4FM modulator ────────────────────────────────────────────────────────────

type C4FM struct {
	poly    [][]float64
	history []float64
	histPos int
	phase   float64
}

func rrcSample(t, T, alpha float64) float64 {
	eps := 1e-9
	if math.Abs(t) < eps {
		return 1.0 - alpha + 4.0*alpha/math.Pi
	}
	crit := T / (4.0 * alpha)
	if math.Abs(math.Abs(t)-crit) < eps {
		return alpha / math.Sqrt2 * ((1+2/math.Pi)*math.Sin(math.Pi/(4*alpha)) +
			(1-2/math.Pi)*math.Cos(math.Pi/(4*alpha)))
	}
	tN := t / T
	num := math.Sin(math.Pi*tN*(1-alpha)) + 4*alpha*tN*math.Cos(math.Pi*tN*(1+alpha))
	den := math.Pi * tN * (1 - (4*alpha*tN)*(4*alpha*tN))
	return num / den
}

func NewC4FM() *C4FM {
	c := &C4FM{
		history: make([]float64, polyTaps),
		poly:    make([][]float64, osr),
	}
	for i := range c.poly {
		c.poly[i] = make([]float64, polyTaps)
	}
	c.buildFilter()
	return c
}

func (c *C4FM) buildFilter() {
	ntaps := rrcSpan*2*osr + 1
	centre := float64(ntaps-1) / 2.0
	T := 1.0 / symbolRate

	h := make([]float64, ntaps)
	for i := 0; i < ntaps; i++ {
		t := (float64(i) - centre) / float64(osr) / symbolRate
		h[i] = rrcSample(t, T, rrcAlpha)
	}
	if h[ntaps/2] != 0 {
		scale := 1.0 / h[ntaps/2]
		for i := range h {
			h[i] *= scale
		}
	}
	// Split into OSR polyphase components
	for p := 0; p < osr; p++ {
		for j := 0; j < polyTaps; j++ {
			idx := j*osr + p
			if idx < ntaps {
				c.poly[p][j] = h[idx]
			}
		}
	}
}

// Modulate converts a dibit slice into interleaved int8 IQ samples.
func (c *C4FM) Modulate(dibits []uint8) []int8 {
	phaseInc := 2.0 * math.Pi * deviationHz / sampleRate
	out := make([]int8, len(dibits)*osr*2)
	outIdx := 0

	for _, d := range dibits {
		sym := symVal[d&3]
		c.history[c.histPos] = sym
		c.histPos = (c.histPos + 1) % polyTaps

		for p := 0; p < osr; p++ {
			acc := 0.0
			comp := c.poly[p]
			for j := 0; j < polyTaps; j++ {
				idx := (c.histPos - 1 - j + polyTaps) % polyTaps
				acc += comp[j] * c.history[idx]
			}
			c.phase += acc * phaseInc
			out[outIdx] = int8(math.Cos(c.phase) * 100)
			outIdx++
			out[outIdx] = int8(math.Sin(c.phase) * 100)
			outIdx++
		}
	}
	return out
}

// GenerateIQ builds `count` repetitions of a P25 TSBK frame as int8 IQ.
// Repetitions are sent back-to-back for faster sync/lock on the receiver.
func GenerateIQ(nac uint16, tsbk [12]byte, count int) []int8 {
	mod := NewC4FM()
	dibits := BuildFrame(nac, tsbk)
	frameIQ := mod.Modulate(dibits)

	total := len(frameIQ) * count
	out := make([]int8, 0, total)
	for i := 0; i < count; i++ {
		out = append(out, frameIQ...)
	}
	return out
}
