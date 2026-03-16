# SiteSim — P25 Phase 1 Site Simulator

## What We're Building
A P25 Phase 1 trunked site simulator for the amateur radio bands.

- **TX (HackRF)** — Transmits a Control Channel and Voice Channels using real
  C4FM/IMBE modulation that real P25 radios can affiliate with and decode.
- **RX (RTL-SDR + dsd-neo)** — Monitors the site's own transmissions and any
  incoming unit traffic; decodes and logs P25 messages in real time.
- **GUI (Dear ImGui)** — Site configuration, active channel display, unit
  roster, and live FFT/waterfall for both TX and RX.

## What P25 Phase 1 Requires

### Physical Layer
| Parameter | Value |
|---|---|
| Modulation | C4FM (4-level FSK) |
| Symbol rate | 4800 baud |
| Bit rate | 9600 bps |
| Channel spacing | 12.5 kHz |
| Levels | ±600 Hz, ±1800 Hz deviation |
| Frame sync | 48-bit NID (incl. DUID + BCH) |

### Frame Types (DUIDs)
| DUID | Name | Use |
|---|---|---|
| 0x0 | HDU | Header Data Unit — starts a voice call |
| 0x3 | TDU | Terminator — ends a call |
| 0x5 | LDU1 | Link Data Unit 1 — 9 IMBE frames + LC |
| 0xA | LDU2 | Link Data Unit 2 — 9 IMBE frames + ES |
| 0xC | TSBK | Trunked System Block — control messages |
| 0xF | TDULC | Terminator with LC |

### Key Identifiers
- **WACN** — Wide Area Communications Network (20-bit), e.g. `0xBEEEE`
- **SYSID** — System ID (12-bit), e.g. `0x001`
- **RFSS** — RF Sub-System ID (8-bit)
- **SITE** — Site ID (8-bit)
- **CC** — Control Channel frequency
- **VC** — Voice Channel frequencies (one per active call)
- **NAC** — Network Access Code (12-bit), used in NID header

### Control Channel TSBK Messages (minimum viable site)
1. **Network Status Broadcast (NSB)** — WACN, SYSID, CC freq, NAC
2. **RFSS Status Broadcast** — RFSS, SITE, CC details, system flags
3. **Adjacent Site Broadcast** — (can be empty for single-site)
4. **Group Voice Channel Grant** — in response to PTT request
5. **Unit Registration Response** — acknowledge unit affiliation

### Voice Call Sequence
```
HDU (header, 1 frame)
LDU1 × N  (9 IMBE blocks each)
LDU2 × N  (9 IMBE blocks each)
TDULC (terminator)
```

## Stack
| Layer | Technology |
|---|---|
| UI | Dear ImGui (docking branch) — reuse from rfstudio |
| TX hardware | SoapySDR → HackRF |
| RX hardware | librtlsdr (direct) or SoapySDR RX |
| P25 decoder | dsd-neo (embedded library) @ ~/Compiled/dsd-neo |
| IMBE vocoder | mbelib-neo @ ~/Compiled/mbelib-neo |
| Modulation | Custom C4FM encoder (baseband → IQ) |
| FFT | FFTW3 |
| Build | CMake / C++20 |

## Project Structure

```
SiteSim/
├── CMakeLists.txt
├── plan.md
├── third_party/
│   ├── imgui/          ← copy/symlink from rfstudio
│   └── nlohmann/       ← copy/symlink from rfstudio
├── src/
│   ├── main.cpp
│   ├── p25/
│   │   ├── NID.hpp/.cpp        ← Network ID: DUID + NAC → 64-bit NID word + BCH(63,16) ECC
│   │   ├── C4FM.hpp/.cpp       ← Baseband: dibits → {±600, ±1800 Hz} → IQ at configured SR
│   │   ├── TSBK.hpp/.cpp       ← Encode/decode TSBK PDUs (OSP: NSB, RFSS, GVCHG, URR...)
│   │   ├── VoiceFrame.hpp/.cpp ← HDU / LDU1 / LDU2 / TDULC builders
│   │   └── IMBECodec.hpp/.cpp  ← Thin wrapper around mbelib-neo encode/decode
│   ├── tx/
│   │   ├── SoapyTx.hpp/.cpp       ← SoapySDR TX sink (HackRF), same pattern as rfstudio
│   │   ├── ControlChannel.hpp/.cpp ← CC scheduler: broadcasts NSB/RFSS every ~1s, handles grants
│   │   └── VoiceChannel.hpp/.cpp   ← VC: sequences HDU→LDU1→LDU2→TDULC, feeds IMBE encoder
│   ├── rx/
│   │   ├── RtlReceiver.hpp/.cpp   ← RTL-SDR raw IQ → FM demod → dsd-neo pipeline
│   │   └── UnitMonitor.hpp/.cpp   ← Parses dsd-neo output, maintains unit/talkgroup table
│   └── ui/
│       ├── App.hpp/.cpp           ← Main window, docking layout, DockBuilder pre-dock
│       ├── SitePanel.hpp/.cpp     ← Site config: WACN, SYSID, NAC, CC freq, VC freqs
│       ├── ChannelPanel.hpp/.cpp  ← Active channel list, affiliated units, call log
│       └── SpectrumPanel.hpp/.cpp ← FFTW3 waterfall for TX IQ and RX IQ side-by-side
```

## Implementation Phases

### Phase 1 — P25 Physical Layer
- C4FM encoder: maps dibits to {-1800, -600, +600, +1800} Hz deviation
- NID encoder: pack DUID + NAC, apply BCH(63,16) + parity
- TSBK encoder: NSB and RFSS Status Broadcast (minimum for a radio to see the site)
- Verify with dsd-neo decoding the output

### Phase 2 — Control Channel TX
- SoapySDR TX sink (reuse rfstudio pattern)
- ControlChannel: periodic TSBK scheduler (NSB every 1s, RFSS every 1s, alternating)
- HackRF transmits a live P25 CC — real radios should see "P25 System XXXX"

### Phase 3 — RX + dsd-neo integration
- RTL-SDR receiver (librtlsdr, FM demod at 12.5 kHz)
- Feed IQ into dsd-neo engine as a stream
- Parse decoded output for unit registrations and voice requests
- UnitMonitor maintains affiliated unit table

### Phase 4 — Voice Channel
- IMBE encoder via mbelib-neo (encode PCM audio → IMBE frames)
- VoiceChannel: HDU → LDU1/LDU2 loop → TDULC
- On PTT (button or audio threshold): grant VC via TSBK, spin up voice TX

### Phase 5 — GUI
- Site config panel (editable WACN/SYSID/NAC/freqs, save/load JSON)
- Channel panel (active VCs, affiliated units, call log)
- Dual waterfall (TX left, RX right)
- TX/RX status indicators

## Amateur Radio Notes
- Must hold an amateur radio license to transmit
- Suitable bands: 70cm (420–450 MHz), 2m (144–148 MHz)
- P25 is permitted on amateur bands for digital voice experimentation
- Keep ERP reasonable; coordinate with local frequency coordinator if desired
- HackRF max output: ~10–15 dBm; add PA if needed for range

## Key References
- TIA-102.BAAA — P25 CAI standard (C4FM modulation)
- TIA-102.AABB — IMBE vocoder
- ~/Compiled/dsd-neo — decoder source (protocol reference)
- ~/Compiled/mbelib-neo — IMBE codec

## ccemu — Go Reference Implementation

`~/Compiled/ccemu` is a working P25 Phase 1 CC transmitter in Go, also by this
author. **The entire P25 encoding stack can be ported 1:1 to C++.** Every
algorithm is a direct translation:

| Go file | C++ target | Notes |
|---|---|---|
| `p25/bch.go` | `src/p25/NID.hpp` | 16-entry uint64 generator matrix, XOR fold |
| `p25/crc.go` | `src/p25/TSBK.hpp` | Direct-form CRC-CCITT, poly 0x1021, XOR 0xFFFF |
| `p25/trellis.go` | `src/p25/TSBK.hpp` | 4×4×2 trellis table + data interleave |
| `p25/frame.go` | `src/p25/Frame.hpp` | FrameSync=0x5575F5FF77FF, status symbol insertion |
| `p25/tsbk.go` | `src/p25/TSBK.hpp` | All TSBK PDU builders (bit-packing only) |
| `dsp/c4fm.go` | `src/p25/C4FM.hpp` | Polyphase RRC (α=0.2, OSR=500), FM integration → int8 IQ |
| `main.go` | `src/tx/ControlChannel.cpp` | Ring buffer, frame scheduler, simulation goroutine |

### HackRF TX pattern
ccemu uses libhackrf directly. SiteSim uses **SoapySDR with CS8 format** —
same int8 IQ output, no conversion, works with other devices too:

```cpp
// CS8 = interleaved signed int8 IQ — matches C4FM output exactly
stream = dev->setupStream(SOAPY_SDR_TX, "CS8");
dev->writeStream(stream, ptrs, 4096, flags, timeNs, 100000);
```

### C4FM modulator key parameters
- Sample rate: **2.4 Msps** (OSR=500, 4800 baud)
- Polyphase RRC: α=0.2, 4-symbol span, 4001 total taps, 9 taps/phase
- FM deviation: ±600 Hz (inner) / ±1800 Hz (outer)
- Dibit→symbol map: `{00→+1, 01→+3, 10→-1, 11→-3}`
- Output: signed int8 I, signed int8 Q

### Frame scheduler (from ccemu main.go)
```
Every 4th frame: system broadcast cycle (IDEN_UP → NET_STS → RFSS_STS → ADJ_STS)
Other frames:    activity queue (grants, affiliations, registrations)
                 → fall back to next system broadcast if queue empty
Ring buffer:     4 MB (~0.83s at 2.4 Msps), producer thread ↔ TX thread
```

## Local Dependencies
| Library | Path | Status |
|---|---|---|
| dsd-neo | ~/Compiled/dsd-neo | cloned, needs build |
| mbelib-neo | ~/Compiled/mbelib-neo | cloned, needs build |
| SoapySDR + HackRF | system | installed |
| librtlsdr | system | installed |
| FFTW3 | system | installed |
| Dear ImGui | ~/Compiled/rfstudio/third_party/imgui | reuse |
