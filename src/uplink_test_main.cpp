#include "rx/RxPipeline.hpp"

#include <atomic>
#include <csignal>
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
        << "Listens on an RTL-SDR and logs ISP/uplink decodes to stdout.\n\n"
        << "Options:\n"
        << "  --rx-freq <MHz>   RTL-SDR center frequency in MHz (default: 145.6500)\n"
        << "  --nac <hex>       NAC in hex without 0x prefix (default: 293)\n"
        << "  --sysid <hex>     SysID in hex without 0x prefix (default: 001)\n"
        << "  --help            Show this help\n\n"
        << "Example:\n"
        << "  " << argv0 << " --rx-freq 145.65 --nac 293 --sysid 001\n";
}

enum class ParseResult {
    Ok,
    Help,
    Error
};

ParseResult parseArgs(int argc, char** argv, SiteConfig& cfg)
{
    cfg.ccFreqHz = 145.650e6;
    cfg.txOffsetMHz = 0.0;
    cfg.nac = 0x293;
    cfg.sysid = 0x001;

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
    try {
        switch (parseArgs(argc, argv, cfg)) {
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

    while (!g_stop.load() && rx.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    rx.stop();
    return 0;
}
