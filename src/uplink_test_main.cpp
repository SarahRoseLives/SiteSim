#include "rx/RxPipeline.hpp"

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

        std::cerr << "Unknown option: " << arg << "\n";
        printUsage(argv[0]);
        return ParseResult::Error;
    }

    return ParseResult::Ok;
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
