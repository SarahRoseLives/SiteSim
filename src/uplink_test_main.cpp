#include "rx/RxPipeline.hpp"
#include "rx/P25Receiver.hpp"
#include "rx/VoiceRepeater.hpp"
#include "p25/C4FM.hpp"
#include "p25/Frame.hpp"
#include "p25/TSBK.hpp"

#include <cstdio>
#include <cstring>
#include <complex>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <rtl-sdr.h>

#ifdef SITESIM_HAVE_DSDNEO
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/io/rtl_stream_c.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop.store(true);
}

void printUsage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Standalone P25 uplink receiver test for SiteSim's dsd-neo path.\n"
        << "Listens on an RTL-SDR (or SoapySDR device) and logs ISP/uplink decodes.\n\n"
        << "Options:\n"
        << "  --rx-freq <MHz>   RX center frequency in MHz (default: 145.6500)\n"
        << "  --nac <hex>       NAC in hex without 0x prefix (default: 293)\n"
        << "  --sysid <hex>     SysID in hex without 0x prefix (default: 123)\n"
        << "  --pluto           Receive on an ADALM-Pluto via SoapySDR\n"
        << "  --soapy <args>    SoapySDR RX device args (e.g. driver=hackrf)\n"
        << "  --soapy-profile <name>  SoapySDR profile (auto|pluto|hackrf|...)\n"
        << "  --sample-rate <Hz>      Exact SoapySDR RX sample rate\n"
        << "  --shared-tx       Open the TX side of the same device and share the\n"
        << "                    handle with RX (validates full-duplex wiring; no RF\n"
        << "                    is transmitted unless TX is started)\n"
        << "  --key-tx          With --shared-tx: start the control-channel TX too\n"
        << "                    (transmits RF on --rx-freq for --duration seconds)\n"
        << "  --tx-gain <dB>    TX gain for --key-tx (Pluto 0-89, default 60)\n"
        << "  --duration <sec>  Stop after N seconds (0 = until Ctrl-C)\n"
        << "  --selftest        Modulate + decode known TSBKs (no RF)\n"
        << "  --voicetest       Composite CC+voice repeat loopback (no RF)\n"
        << "  --help            Show this help\n\n"
        << "Example:\n"
        << "  " << argv0 << " --rx-freq 145.65 --nac 293 --sysid 123\n";
}

enum class ParseResult {
    Ok,
    Help,
    Error
};

struct RxSource {
    std::string soapyArgs;
    std::string soapyProfile;
    uint32_t    sampleRateHz = 0;
    bool        sharedTx     = false;
    bool        keyTx        = false;
    double      txGainDb     = 60.0;
    int         durationSec  = 0;

    // Offline IQ decode (no SDR hardware needed).
    std::string iqPath;
    uint32_t    iqRate   = 250000;
    bool        iqRaw    = false;   // raw cu8, no WAV header
    int         iqOffset = -1;      // data offset override (-1 = auto)

    // Native (in-process) P25Receiver live decode via SoapySDR RX.
    bool        native   = false;
    double      rxGainDb = -1.0;
    bool        invert   = false;
    std::string recordPath;      // write raw CF32 IQ instead of/while decoding
    bool        iqCf32   = false;  // offline reader: file is raw float32 I/Q
    bool        selftest = false;  // modulate known TSBKs -> decode (no RF)
    bool        voicetest = false; // composite CC+voice repeat loopback (no RF)
    int         ssInterval = 35;   // OSP status symbol interval (frame dibits)
    int         mixHz = 0;         // baseband downconvert for off-centre IQ
    double      gardner = 0.00015; // Gardner timing loop gain
    int         fswDist = 8;       // max FSW Hamming distance to accept
    bool        rtlNative = false; // capture via librtlsdr directly (proven path)
    int         rtlOffsetHz = 0;   // tune offset to dodge RTL DC spike
};

ParseResult parseArgs(int argc, char** argv, SiteConfig& cfg, RxSource& rx)
{
    cfg.ccFreqHz = 145.650e6;
    cfg.txOffsetMHz = 0.0;
    cfg.nac = 0x293;
    cfg.sysid = 0x123;
    // The standalone receiver is single-frequency; drop the UI channel plan so
    // normalizeChannelPlan() builds a one-channel plan from --rx-freq.
    cfg.channels.clear();
    cfg.controlIndex = 0;
    cfg.voiceIndex   = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto requireValue = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return ParseResult::Help;
        }
        if (arg == "--rx-freq") {
            const char* value = requireValue("--rx-freq");
            if (!value) return ParseResult::Error;
            cfg.ccFreqHz = std::stod(value) * 1e6;
            cfg.txOffsetMHz = 0.0;
            continue;
        }
        if (arg == "--nac") {
            const char* value = requireValue("--nac");
            if (!value) return ParseResult::Error;
            cfg.nac = static_cast<uint16_t>(std::stoul(value, nullptr, 16) & 0xFFFu);
            continue;
        }
        if (arg == "--sysid") {
            const char* value = requireValue("--sysid");
            if (!value) return ParseResult::Error;
            cfg.sysid = static_cast<uint16_t>(std::stoul(value, nullptr, 16) & 0xFFFu);
            continue;
        }
        if (arg == "--pluto") {
            rx.soapyArgs = "driver=plutosdr";
            rx.soapyProfile = "pluto";
            rx.sampleRateHz = 1536000;
            continue;
        }
        if (arg == "--soapy") {
            const char* value = requireValue("--soapy");
            if (!value) return ParseResult::Error;
            rx.soapyArgs = value;
            continue;
        }
        if (arg == "--soapy-profile") {
            const char* value = requireValue("--soapy-profile");
            if (!value) return ParseResult::Error;
            rx.soapyProfile = value;
            continue;
        }
        if (arg == "--sample-rate") {
            const char* value = requireValue("--sample-rate");
            if (!value) return ParseResult::Error;
            rx.sampleRateHz = static_cast<uint32_t>(std::stoul(value));
            continue;
        }
        if (arg == "--shared-tx") {
            rx.sharedTx = true;
            continue;
        }
        if (arg == "--key-tx") {
            rx.keyTx = true;
            continue;
        }
        if (arg == "--tx-gain") {
            const char* value = requireValue("--tx-gain");
            if (!value) return ParseResult::Error;
            rx.txGainDb = std::stod(value);
            continue;
        }
        if (arg == "--duration") {
            const char* value = requireValue("--duration");
            if (!value) return ParseResult::Error;
            rx.durationSec = std::stoi(value);
            continue;
        }
        if (arg == "--iq") {
            const char* value = requireValue("--iq");
            if (!value) return ParseResult::Error;
            rx.iqPath = value;
            continue;
        }
        if (arg == "--iq-rate") {
            const char* value = requireValue("--iq-rate");
            if (!value) return ParseResult::Error;
            rx.iqRate = static_cast<uint32_t>(std::stoul(value));
            continue;
        }
        if (arg == "--iq-raw") {
            rx.iqRaw = true;
            continue;
        }
        if (arg == "--native") {
            rx.native = true;
            continue;
        }
        if (arg == "--invert") {
            rx.invert = true;
            continue;
        }
        if (arg == "--rx-gain") {
            const char* value = requireValue("--rx-gain");
            if (!value) return ParseResult::Error;
            rx.rxGainDb = std::stod(value);
            continue;
        }
        if (arg == "--record") {
            const char* value = requireValue("--record");
            if (!value) return ParseResult::Error;
            rx.recordPath = value;
            continue;
        }
        if (arg == "--iq-cf32") {
            rx.iqCf32 = true;
            continue;
        }
        if (arg == "--selftest") {
            rx.selftest = true;
            continue;
        }
        if (arg == "--voicetest") {
            rx.voicetest = true;
            continue;
        }
        if (arg == "--ss") {
            const char* value = requireValue("--ss");
            if (!value) return ParseResult::Error;
            rx.ssInterval = std::stoi(value);
            continue;
        }
        if (arg == "--mix") { const char* value = requireValue("--mix"); if (!value) return ParseResult::Error; rx.mixHz = std::stoi(value); continue; }
        if (arg == "--gardner") { const char* value = requireValue("--gardner"); if (!value) return ParseResult::Error; rx.gardner = std::stod(value); continue; }
        if (arg == "--fswdist") { const char* value = requireValue("--fswdist"); if (!value) return ParseResult::Error; rx.fswDist = std::stoi(value); continue; }

        if (arg == "--rtl-native") {
            rx.rtlNative = true;
            continue;
        }
        if (arg == "--rtl-offset") {
            const char* value = requireValue("--rtl-offset");
            if (!value) return ParseResult::Error;
            rx.rtlOffsetHz = std::stoi(value);
            continue;
        }

        std::cerr << "Unknown option: " << arg << "\n";
        printUsage(argv[0]);
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

int runOfflineIq(const RxSource& rx)
{
    std::ifstream f(rx.iqPath, std::ios::binary);
    if (!f) { std::cerr << "Cannot open " << rx.iqPath << "\n"; return 1; }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (raw.size() < 8) { std::cerr << "file too small\n"; return 1; }

    size_t off = 0;
    if (!rx.iqRaw && !rx.iqCf32) {
        if (raw.size() > 12 && std::memcmp(raw.data(), "RIFF", 4) == 0) {
            for (size_t i = 12; i + 8 < raw.size() && i < 256; ++i) {
                if (std::memcmp(raw.data() + i, "data", 4) == 0) { off = i + 8; break; }
            }
        } else {
            off = 44;
        }
    }
    if (off >= raw.size()) off = 0;

    std::cout << "Offline IQ decode: " << rx.iqPath
              << "  rate=" << rx.iqRate << " Hz  dataOffset=" << off
              << "  bytes=" << raw.size() << "\n";

    p25rx::P25Receiver recv;
    recv.configure(double(rx.iqRate), 0);
    recv.setSsInterval(rx.ssInterval);
    recv.setMixOffset(double(rx.mixHz));
    recv.setGardnerGain(rx.gardner);
    recv.setFswMaxDist(rx.fswDist);
    uint64_t count = 0;
    recv.setTsbkCallback([&](const std::array<uint8_t, 12>& t) {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "TSBK op=0x%02X raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      t[0] & 0x3F, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                      t[8], t[9], t[10], t[11]);
        std::cout << line << "\n";
        ++count;
    });

    std::vector<float> buf;
    buf.reserve(8192);
    if (rx.iqCf32) {
        const size_t nf = (raw.size() - off) / sizeof(float);
        const float* f = reinterpret_cast<const float*>(raw.data() + off);
        for (size_t k = 0; k < nf; ++k) {
            buf.push_back(f[k]);
            if (buf.size() >= 8192) { recv.process(buf.data(), buf.size() / 2); buf.clear(); }
        }
    } else {
        for (size_t i = 0; i + 1 < raw.size() - off; i += 2) {
            buf.push_back((float(raw[off + i])     - 127.5f) / 127.5f);
            buf.push_back((float(raw[off + i + 1]) - 127.5f) / 127.5f);
            if (buf.size() >= 8192) { recv.process(buf.data(), buf.size() / 2); buf.clear(); }
        }
    }
    if (!buf.empty()) recv.process(buf.data(), buf.size() / 2);

    auto& s = recv.stats();
    std::cout << "samples=" << s.samples << " symbols=" << s.symbols
              << " fswHits=" << s.fswHits << " nidOk=" << s.nidOk
              << " framesTried=" << s.framesTried << " crcOk=" << s.crcOk
              << " tsbks=" << s.tsbks << "\n";
    std::cout << "minFswDist=" << s.minFswDist << " minFswDistInv=" << s.minFswDistInv
              << " dibits[" << s.dibitCount[0] << "," << s.dibitCount[1]
              << "," << s.dibitCount[2] << "," << s.dibitCount[3] << "]\n";
    std::cout << "symRange=[" << s.symMin << "," << s.symMax << "]\n";
    std::cout << "symHist(-6..+6):";
    for (int b = 0; b < 24; ++b) std::cout << " " << (b - 12) << ":" << s.symHist[b];
    std::cout << "\n";
    return count ? 0 : 2;
}

int runNativeLive(const RxSource& rx, const SiteConfig& cfg)
{
    std::string args = rx.soapyArgs.empty() ? "driver=rtlsdr" : rx.soapyArgs;
    uint32_t rate = rx.sampleRateHz ? rx.sampleRateHz : 1536000;
    double freq = cfg.rxFreqHz();

    SoapySDR::Device* dev = nullptr;
    try { dev = SoapySDR::Device::make(args); }
    catch (const std::exception& e) { std::cerr << "Soapy make failed: " << e.what() << "\n"; return 1; }
    if (!dev) { std::cerr << "No Soapy device\n"; return 1; }

    SoapySDR::Stream* stream = nullptr;
    try {
        dev->setSampleRate(SOAPY_SDR_RX, 0, double(rate));
        dev->setFrequency(SOAPY_SDR_RX, 0, freq);
        try {
            if (rx.rxGainDb < 0.0) dev->setGainMode(SOAPY_SDR_RX, 0, true);   // AGC
            else                  dev->setGain(SOAPY_SDR_RX, 0, rx.rxGainDb);
        } catch (...) {}
        try { dev->setBandwidth(SOAPY_SDR_RX, 0, double(rate)); } catch (...) {}
        stream = dev->setupStream(SOAPY_SDR_RX, "CF32");
        if (!stream) throw std::runtime_error("setupStream failed");
        dev->activateStream(stream);
    } catch (const std::exception& e) {
        std::cerr << "Soapy RX config failed: " << e.what() << "\n";
        SoapySDR::Device::unmake(dev);
        return 1;
    }

    p25rx::P25Receiver recv;
    recv.configure(double(rate), cfg.nac);
    recv.setSsInterval(rx.ssInterval);
    recv.setInvert(rx.invert);
    recv.setGardnerGain(rx.gardner);
    recv.setFswMaxDist(rx.fswDist);
    recv.setMixOffset(double(rx.rtlOffsetHz));
    uint64_t count = 0;
    recv.setTsbkCallback([&](const std::array<uint8_t, 12>& t) {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "TSBK op=0x%02X raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      t[0] & 0x3F, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                      t[8], t[9], t[10], t[11]);
        std::cout << line << "\n" << std::flush;
        ++count;
    });

    std::cout << "Native P25Receiver RX on " << std::fixed << std::setprecision(4)
              << (freq / 1e6) << " MHz  rate=" << rate << "  args=" << args
              << "  gain=" << rx.rxGainDb << "\n" << std::flush;

    std::vector<std::complex<float>> buff(16384);
    std::vector<float> inter;
    double rmsAcc = 0.0; uint64_t rmsN = 0;
    std::ofstream rec;
    if (!rx.recordPath.empty()) {
        rec.open(rx.recordPath, std::ios::binary);
        std::cout << "Recording raw CF32 IQ to " << rx.recordPath << "\n" << std::flush;
    }
    const int duration = rx.durationSec > 0 ? rx.durationSec : 20;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);

    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        void* bufs[1] = { buff.data() };
        int flags = 0; long long ts = 0;
        int n = dev->readStream(stream, bufs, buff.size(), flags, ts, 200000);
        if (n > 0) {
            inter.resize(size_t(n) * 2);
            for (int i = 0; i < n; ++i) {
                inter[2 * i]     = buff[i].real();
                inter[2 * i + 1] = buff[i].imag();
                rmsAcc += double(buff[i].real()) * buff[i].real() + double(buff[i].imag()) * buff[i].imag();
                ++rmsN;
            }
            if (rec) rec.write(reinterpret_cast<const char*>(inter.data()),
                               std::streamsize(inter.size() * sizeof(float)));
            recv.process(inter.data(), size_t(n));
        }
    }

    if (rec) rec.close();

    dev->deactivateStream(stream);
    dev->closeStream(stream);
    SoapySDR::Device::unmake(dev);

    if (rmsN) std::cout << "IQ rms=" << std::sqrt(rmsAcc / double(rmsN)) << "\n";
    auto& s = recv.stats();
    std::cout << "samples=" << s.samples << " symbols=" << s.symbols
              << " fswHits=" << s.fswHits << " nidOk=" << s.nidOk
              << " framesTried=" << s.framesTried << " crcOk=" << s.crcOk
              << " tsbks=" << s.tsbks
              << " minFswDist=" << s.minFswDist << "/" << s.minFswDistInv << "\n";
    return 0;
}

int runSelfTest()
{
    const double rate = 1536000.0;
    p25::C4FM mod;
    if (!mod.configure(rate)) { std::cerr << "mod configure failed\n"; return 1; }
    mod.setDeviationPeakHz(1800.0);

    std::vector<std::array<uint8_t, 12>> sent;
    std::vector<uint8_t> dibits;
    auto add = [&](const std::array<uint8_t, 12>& t) {
        sent.push_back(t);
        auto d = p25::BuildFrame(0x123, t);
        dibits.insert(dibits.end(), d.begin(), d.end());
    };
    add(p25::BuildURegRsp(true, 0x123, 1, 1));
    add(p25::BuildGrpAffRsp(false, 0, 1, 1, 1));
    add(p25::BuildGrpVChGrant(0, 1, 1, 1, 1));
    add(p25::BuildNetStatusBcast(0xBEEF0, 0x123, 1, 0, 0x70));
    add(p25::BuildIDENUp(1, 100, 0, 100, 438800000ull));
    for (int k = 0; k < 5; ++k) add(sent[0]);   // repeat U_REG for margin

    auto iq8 = mod.modulate(dibits);
    std::cout << "selftest: " << sent.size() << " frames, " << iq8.size() / 2
              << " samples generated\n";

    p25rx::P25Receiver recv;
    recv.configure(rate, 0);
    uint64_t ok = 0;
    recv.setTsbkCallback([&](const std::array<uint8_t, 12>& t) {
        std::printf("  decoded TSBK op=0x%02X raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    t[0] & 0x3F, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                    t[8], t[9], t[10], t[11]);
        ++ok;
    });

    std::vector<float> buf;
    buf.reserve(8192);
    for (size_t k = 0; k + 1 < iq8.size(); k += 2) {
        buf.push_back(iq8[k] / 100.0f);
        buf.push_back(iq8[k + 1] / 100.0f);
        if (buf.size() >= 8192) { recv.process(buf.data(), buf.size() / 2); buf.clear(); }
    }
    if (!buf.empty()) recv.process(buf.data(), buf.size() / 2);

    auto& s = recv.stats();
    std::cout << "selftest stats: symbols=" << s.symbols << " fswHits=" << s.fswHits
              << " nidOk=" << s.nidOk << " framesTried=" << s.framesTried
              << " crcOk=" << s.crcOk << " tsbks=" << s.tsbks
              << " minFswDist=" << s.minFswDist << "\n";
    std::cout << "selftest: decoded " << ok << " / " << sent.size() << " frames\n";
    return ok ? 0 : 2;
}

int runVoiceTest()
{
    // Composite CC + voice loopback: generate independent CC and VC frames,
    // mix them to opposite offsets around a shared centre, and prove that the
    // voice repeater recovers and re-modulates the VC frame while the CC
    // receiver still decodes the CC frame through the adjacent-channel
    // interference. No SDR hardware is involved.
    const double rate   = 1536000.0;
    const double offset = 250e3;
    const uint16_t nac  = 0x293;

    const auto ccTsbk = p25::BuildURegRsp(true, 0x123, 0x111111, 0x111111);
    const auto vcTsbk = p25::BuildURegRsp(true, 0x123, 0x222222, 0x222222);

    p25::C4FM ccMod, vcMod;
    if (!ccMod.configure(rate) || !vcMod.configure(rate)) {
        std::cerr << "C4FM configure failed\n";
        return 1;
    }
    ccMod.setDeviationPeakHz(1800.0);
    vcMod.setDeviationPeakHz(1800.0);

    auto ccIq = ccMod.modulate(p25::BuildFrame(nac, ccTsbk));
    auto vcIq = vcMod.modulate(p25::BuildFrame(nac, vcTsbk));

    const size_t n     = std::max(ccIq.size(), vcIq.size());
    const size_t pad   = size_t(rate * 0.05);
    const size_t total = n + pad;

    std::vector<float> composite(total * 2, 0.0f);
    const double inc = 2.0 * M_PI * offset / rate;
    std::complex<double> ccRot(1.0, 0.0), ccStep(std::cos(inc), std::sin(inc));
    std::complex<double> vcRot(1.0, 0.0), vcStep(std::cos(-inc), std::sin(-inc));
    for (size_t i = 0; i < total; ++i) {
        float ci = 0, cq = 0, vi = 0, vq = 0;
        const size_t j = (i >= pad / 2) ? i - pad / 2 : SIZE_MAX;
        if (j != SIZE_MAX && j < ccIq.size() / 2) {
            ci = float(ccIq[2 * j]) / 127.0f;
            cq = float(ccIq[2 * j + 1]) / 127.0f;
        }
        if (j != SIZE_MAX && j < vcIq.size() / 2) {
            vi = float(vcIq[2 * j]) / 127.0f;
            vq = float(vcIq[2 * j + 1]) / 127.0f;
        }
        composite[2 * i]     = float(ci * ccRot.real() - cq * ccRot.imag()) +
                               float(vi * vcRot.real() - vq * vcRot.imag());
        composite[2 * i + 1] = float(ci * ccRot.imag() + cq * ccRot.real()) +
                               float(vi * vcRot.imag() + vq * vcRot.real());
        ccRot *= ccStep;
        vcRot *= vcStep;
    }

    p25rx::VoiceRepeater vr;
    if (!vr.configure(rate, +offset)) { std::cerr << "voice configure failed\n"; return 2; }
    for (size_t off = 0; off < total; off += 4096) {
        size_t c = std::min<size_t>(4096, total - off);
        vr.process(composite.data() + off * 2, c);
    }

    // Re-modulate the recovered discriminator exactly as ControlChannel does.
    std::vector<float> vcOut(total * 2, 0.0f);
    {
        const double step         = vr.demodRate() / rate;
        const double phasePerUnit = 2.0 * M_PI * 600.0 / rate;
        const double offsetInc    = 2.0 * M_PI * (-offset) / rate;
        double pos = 0.0, phase = 0.0;
        float prev = 0.0f, next = 0.0f;
        for (size_t i = 0; i < total; ++i) {
            const float s = prev + (next - prev) * float(pos);
            pos += step;
            while (pos >= 1.0) {
                pos -= 1.0;
                prev = next;
                float x = 0.0f;
                next = vr.readFm(x) ? x : prev;
            }
            phase += s * phasePerUnit + offsetInc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            else if (phase < -2.0 * M_PI) phase += 2.0 * M_PI;
            vcOut[2 * i]     = float(std::cos(phase)) * 0.5f;
            vcOut[2 * i + 1] = float(std::sin(phase)) * 0.5f;
        }
    }

    auto decode = [&](const float* iq, double mix, std::array<uint8_t, 12>& got) {
        p25rx::P25Receiver rx;
        rx.configure(rate, nac);
        rx.setMixOffset(mix);
        bool have = false;
        rx.setTsbkCallback([&](const std::array<uint8_t, 12>& t) {
            if (!have) { got = t; have = true; }
        });
        for (size_t off = 0; off < total; off += 2048) {
            size_t k = std::min<size_t>(2048, total - off);
            rx.process(iq + off * 2, k);
        }
        return have;
    };

    std::array<uint8_t, 12> vcGot{}, ccGot{};
    const bool vcOk = decode(vcOut.data(), +offset, vcGot);
    const bool ccOk = decode(composite.data(), -offset, ccGot);
    const bool vcMatch = vcOk && std::memcmp(vcGot.data(), vcTsbk.data(), 12) == 0;
    const bool ccMatch = ccOk && std::memcmp(ccGot.data(), ccTsbk.data(), 12) == 0;

    std::cout << "voicetest: voice decode=" << vcOk << " match=" << vcMatch
              << "  cc decode=" << ccOk << " match=" << ccMatch
              << "  squelch=" << (vr.squelchOpen() ? 1 : 0) << "\n";
    const bool pass = vcMatch && ccMatch;
    std::cout << "voicetest: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 2;
}

int runNativeRtl(const RxSource& rx, const SiteConfig& cfg)
{
    rtlsdr_dev_t* dev = nullptr;
    if (rtlsdr_open(&dev, 0) < 0) { std::cerr << "rtlsdr_open failed (device busy?)\n"; return 1; }
    const uint32_t rate = 1536000;
    rtlsdr_set_sample_rate(dev, rate);
    long center = long(cfg.rxFreqHz()) + rx.rtlOffsetHz;
    if (center < 0) center = 0;
    rtlsdr_set_center_freq(dev, uint32_t(center));
    const bool agc = rx.rxGainDb < 0.0;
    rtlsdr_set_tuner_gain_mode(dev, agc ? 1 : 0);
    if (!agc) rtlsdr_set_tuner_gain(dev, int(rx.rxGainDb * 10.0));
    rtlsdr_reset_buffer(dev);

    p25rx::P25Receiver recv;
    recv.configure(double(rate), cfg.nac);
    recv.setSsInterval(rx.ssInterval);
    recv.setInvert(rx.invert);
    recv.setGardnerGain(rx.gardner);
    recv.setFswMaxDist(rx.fswDist);
    recv.setMixOffset(double(rx.rtlOffsetHz));
    uint64_t cnt = 0;
    recv.setTsbkCallback([&](const std::array<uint8_t, 12>& t) {
        std::printf("TSBK op=0x%02X raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    t[0] & 0x3F, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                    t[8], t[9], t[10], t[11]);
        std::fflush(stdout);
        ++cnt;
    });

    std::cout << "Native librtlsdr RX on " << std::fixed << std::setprecision(4)
              << (cfg.rxFreqHz() / 1e6) << " MHz (offset " << rx.rtlOffsetHz
              << " Hz) rate=" << rate << " agc=" << (agc ? 1 : 0) << "\n" << std::flush;

    std::vector<uint8_t> buf(32768);
    std::vector<float> f;
    std::ofstream rec;
    if (!rx.recordPath.empty()) { rec.open(rx.recordPath, std::ios::binary);
        std::cout << "Recording CF32 to " << rx.recordPath << "\n" << std::flush; }
    double rmsAcc = 0.0; uint64_t rmsN = 0;
    const int duration = rx.durationSec > 0 ? rx.durationSec : 20;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        int n = 0;
        if (rtlsdr_read_sync(dev, buf.data(), int(buf.size()), &n) != 0 || n <= 0) break;
        int ns = n / 2;
        f.resize(size_t(ns) * 2);
        for (int i = 0; i < ns; ++i) {
            float I = (float(buf[2 * i])     - 127.5f) / 127.5f;
            float Q = (float(buf[2 * i + 1]) - 127.5f) / 127.5f;
            f[2 * i] = I; f[2 * i + 1] = Q;
            rmsAcc += double(I) * I + double(Q) * Q; ++rmsN;
        }
        if (rec) rec.write(reinterpret_cast<const char*>(f.data()),
                           std::streamsize(f.size() * sizeof(float)));
        recv.process(f.data(), size_t(ns));
    }
    if (rec) rec.close();
    rtlsdr_close(dev);

    auto& s = recv.stats();
    if (rmsN) std::cout << "IQ rms=" << std::sqrt(rmsAcc / double(rmsN)) << "\n";
    std::cout << "samples=" << s.samples << " symbols=" << s.symbols
              << " fswHits=" << s.fswHits << " nidOk=" << s.nidOk
              << " framesTried=" << s.framesTried << " crcOk=" << s.crcOk
              << " tsbks=" << s.tsbks << " minFswDist=" << s.minFswDist
              << "/" << s.minFswDistInv << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    SiteConfig cfg;
    RxSource rxSource;
    try {
        switch (parseArgs(argc, argv, cfg, rxSource)) {
        case ParseResult::Ok:
            break;
        case ParseResult::Help:
            return 0;
        case ParseResult::Error:
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Argument error: " << ex.what() << "\n";
        printUsage(argv[0]);
        return 1;
    }

    if (rxSource.selftest)
        return runSelfTest();

    if (rxSource.voicetest)
        return runVoiceTest();

    if (!rxSource.iqPath.empty())
        return runOfflineIq(rxSource);

    if (rxSource.rtlNative)
        return runNativeRtl(rxSource, cfg);

    if (rxSource.native)
        return runNativeLive(rxSource, cfg);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SoapyTx txDummy;
    ControlChannel cc(txDummy);
    cc.configure(cfg);

    RxPipeline rx(cc, [](const std::string& msg) {
        std::cout << msg << std::endl;
    });
    rx.configure(cfg);
    if (rxSource.sharedTx) {
        if (rxSource.soapyArgs.empty()) {
            std::cerr << "--shared-tx requires --pluto or --soapy <args>\n";
            return 1;
        }
        const uint32_t rate = rxSource.sampleRateHz ? rxSource.sampleRateHz : 2400000;
        txDummy.setSampleRate(double(rate));
        if (rxSource.keyTx) {
            txDummy.setFrequency(cfg.ccFreqHz);
            txDummy.setGain(rxSource.txGainDb);
        }
        if (!txDummy.open(rxSource.soapyArgs)) {
            std::cerr << "TX open failed: " << txDummy.lastError() << "\n";
            return 1;
        }
        if (rxSource.keyTx && !txDummy.frequencyValid()) {
            std::cerr << txDummy.lastError() << "\nRefusing to key TX.\n";
            return 1;
        }
        rx.useSharedSoapyDevice(txDummy.device(), rxSource.soapyArgs,
                                rxSource.soapyProfile, rate);
    } else if (rxSource.keyTx) {
        std::cerr << "--key-tx requires --shared-tx\n";
        return 1;
    } else if (!rxSource.soapyArgs.empty()) {
        rx.useSoapySource(rxSource.soapyArgs, rxSource.soapyProfile, rxSource.sampleRateHz);
    } else {
        rx.useRtlsdrSource();
    }

    std::cout << "uplink_test listening on "
              << std::fixed << std::setprecision(4) << (cfg.rxFreqHz() / 1e6)
              << " MHz"
              << "  NAC=0x" << std::uppercase << std::hex << cfg.nac
              << "  SYSID=0x" << cfg.sysid << std::dec << "\n"
              << "Press Ctrl-C to stop.\n";

    if (!rx.start()) {
        std::cerr << "Failed to start RxPipeline\n";
        return 1;
    }

    if (rxSource.keyTx) {
        txDummy.startTx();
        cc.start();
        std::cout << "TX keyed on " << std::fixed << std::setprecision(4)
                  << (cfg.ccFreqHz / 1e6) << " MHz @ gain " << rxSource.txGainDb
                  << " dB (no antenna? use a dummy load or attenuator)"
                  << std::endl;

#ifdef SITESIM_HAVE_DSDNEO
        // Diagnostic: query the live shared device and dsd-neo's input level.
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        SoapySDR::Device* dev = txDummy.device();
        if (dev) {
            double rx_c = 0, tx_c = 0, rx_r = 0, tx_r = 0, tx_g = 0, rx_bw = 0;
            try { rx_c = dev->getFrequency(SOAPY_SDR_RX, 0); } catch (...) {}
            try { tx_c = dev->getFrequency(SOAPY_SDR_TX, 0); } catch (...) {}
            try { rx_r = dev->getSampleRate(SOAPY_SDR_RX, 0); } catch (...) {}
            try { tx_r = dev->getSampleRate(SOAPY_SDR_TX, 0); } catch (...) {}
            try { tx_g = dev->getGain(SOAPY_SDR_TX, 0); } catch (...) {}
            try { rx_bw = dev->getBandwidth(SOAPY_SDR_RX, 0); } catch (...) {}
            dsd_input_level_snapshot snap{};
            int rc = rtl_stream_get_input_level(&snap);
            std::cout << "DIAG rx_center=" << (rx_c / 1e6)
                      << " MHz tx_center=" << (tx_c / 1e6)
                      << " MHz rx_rate=" << (rx_r / 1e6)
                      << " tx_rate=" << (tx_r / 1e6)
                      << " rx_bw=" << rx_bw
                      << " tx_gain=" << tx_g
                      << " level_rc=" << rc
                      << " status=" << int(snap.status)
                      << " rms_dbfs=" << snap.rms_dbfs
                      << " peak_dbfs=" << snap.peak_dbfs
                      << " clip_pct=" << snap.clip_pct
                      << " samples=" << snap.sample_count
                      << std::endl;
        }
#endif
    }

    const int duration = rxSource.durationSec > 0 ? rxSource.durationSec
                         : (rxSource.keyTx ? 10 : 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    while (!g_stop.load() && rx.isRunning()) {
        if (duration > 0 && std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    rx.stop();
    cc.stop();
    txDummy.stopTx();
    return 0;
}




