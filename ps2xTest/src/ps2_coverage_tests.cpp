#include "MiniTest.h"
#include "ps2_coverage.h"
#include "ps2_runtime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    constexpr uint32_t kCalled = 0x01E00100u;
    constexpr uint32_t kCalledTwice = 0x01E00200u;
    constexpr uint32_t kNeverCalled = 0x01E00300u;

    void emptyFunction(uint8_t *, R5900Context *, PS2Runtime *) {}

    std::filesystem::path tempCoveragePath()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        return std::filesystem::temp_directory_path() / ("ps2x-coverage-" + suffix + ".txt");
    }

    std::set<uint32_t> readCoverage(const std::filesystem::path &path, std::string &header)
    {
        std::set<uint32_t> pcs;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.rfind("#", 0) == 0)
            {
                header += line + "\n";
                continue;
            }
            if (!line.empty())
                pcs.insert(static_cast<uint32_t>(std::stoul(line, nullptr, 16)));
        }
        return pcs;
    }
}

void register_ps2_coverage_tests()
{
    MiniTest::Case("Coverage", [](TestCase &tc)
                   {
        tc.Run("dispatch records nothing while coverage is off", [](TestCase &t) {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");
            t.IsTrue(runtime.registerFunction(kCalled, &emptyFunction), "register should succeed");
            t.IsFalse(ps2_coverage::active(), "coverage should be off by default");
            runtime.lookupFunction(kCalled);
            t.IsFalse(ps2_coverage::ran(kCalled), "nothing is recorded while off");
            t.IsFalse(ps2_coverage::write(), "write without a session should fail");
        });

        tc.Run("dispatched entries are written once each, others are absent", [](TestCase &t) {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");
            t.IsTrue(runtime.registerFunction(kCalled, &emptyFunction), "register should succeed");
            t.IsTrue(runtime.registerFunction(kCalledTwice, &emptyFunction), "register should succeed");
            t.IsTrue(runtime.registerFunction(kNeverCalled, &emptyFunction), "register should succeed");

            const std::filesystem::path path = tempCoveragePath();
            t.IsTrue(ps2_coverage::start(path.string(), 0u), "start should succeed");
            t.IsFalse(ps2_coverage::start(path.string(), 0u), "a second session should be refused");
            t.IsTrue(ps2_coverage::active(), "coverage should be active");

            runtime.lookupFunction(kCalled);
            runtime.lookupFunction(kCalledTwice);
            ps2_coverage::note(kCalledTwice | 0x80000000u);
            t.IsTrue(ps2_coverage::ran(kCalled), "a looked-up entry is recorded");
            t.IsTrue(ps2_coverage::ran(kCalledTwice), "a KSEG0 alias lands on the same entry");
            t.IsFalse(ps2_coverage::ran(kNeverCalled), "an entry never looked up is not recorded");

            t.IsTrue(ps2_coverage::write(), "write should succeed");
            std::string header;
            std::set<uint32_t> pcs = readCoverage(path, header);
            t.Equals(pcs.size(), static_cast<size_t>(2u), "two distinct PCs should be written");
            t.IsTrue(pcs.count(kCalled) == 1u && pcs.count(kCalledTwice) == 1u, "both dispatched entries are listed");
            t.IsTrue(header.find("ps2x coverage v1") != std::string::npos, "the header names the format");
            t.IsTrue(header.find("# 2 guest PCs") != std::string::npos, "the header gives the count");

            runtime.lookupFunction(kNeverCalled);
            ps2_coverage::stop();
            t.IsFalse(ps2_coverage::active(), "stop ends the session");
            pcs = readCoverage(path, header);
            t.IsTrue(pcs.count(kNeverCalled) == 1u, "stop writes the PCs recorded since the last write");

            runtime.lookupFunction(kCalled);
            t.IsFalse(ps2_coverage::ran(kCalled), "nothing is recorded after stop");
            std::error_code error;
            std::filesystem::remove(path, error);
        });

        tc.Run("the periodic writer updates the file without a stop", [](TestCase &t) {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");
            t.IsTrue(runtime.registerFunction(kCalled, &emptyFunction), "register should succeed");
            const std::filesystem::path path = tempCoveragePath();
            t.IsTrue(ps2_coverage::start(path.string(), 1u), "start should succeed");
            runtime.lookupFunction(kCalled);
            bool written = false;
            for (int i = 0; i < 40 && !written; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::string header;
                written = readCoverage(path, header).count(kCalled) == 1u;
            }
            t.IsTrue(written, "the writer thread should write within a few seconds");
            ps2_coverage::stop();
            std::error_code error;
            std::filesystem::remove(path, error);
        });
    });
}
