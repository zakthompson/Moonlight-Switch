#ifdef __SWITCH2__
#include "Switch2DisplayTiming.hpp"

#include <borealis.hpp>
#include <switch.h>
#include <switch2/services/vi2.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr uint64_t kPollNs = 500000;
constexpr uint64_t kReportNs = 5000000000ull;
constexpr double kRefreshMs = 1000.0 / 120.0;
constexpr int kMaxMultiple = 7; // bins: 1..6 refreshes, then >=7
constexpr double kOffGridMs = 1.5;

uint64_t nowNs() { return armTicksToNs(armGetSystemTick()); }

void logDisplayMode(Vi2Display* display, const char* when) {
    u32 hz = 0;
    bool vrr = false;
    const Result rc = vi2QueryDisplayRefreshRate(display, &hz, &vrr);
    Vi2DisplayModeCapability caps {};
    const Result capsRc = vi2QueryDisplayModeCapability(display, &caps);
    u16 num = 0, den = 0;
    nwindowGetPresentInterval(nwindowGetDefault(), &num, &den);
    brls::Logger::info("FRAME_DISPLAY_MODE: {} refresh={}Hz vrr_active={} rc=0x{:x} refresh_word=0x{:x} "
                       "secondary=0x{:x} caps_rc=0x{:x} present_interval={}/{}",
        when, hz, vrr, rc, caps.refresh_rates, caps.refresh_rates_secondary, capsRc, num, den);
}

} // namespace

Switch2DisplayTiming& Switch2DisplayTiming::instance() {
    static Switch2DisplayTiming timing;
    return timing;
}

void Switch2DisplayTiming::start() {
    stop();
    m_running = true;
    m_thread = std::thread([this] { run(); });
}

void Switch2DisplayTiming::stop() {
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

void Switch2DisplayTiming::run() {
    // Keep the poller off the streaming threads' cores (1-3) when the process
    // may use core 4; it sleeps between short IPC reads.
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && (mask & (1u << 4)))
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, 4, u32(mask));
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);

    Vi2Layer* layer = nullptr;
    Vi2Display* display = nullptr;
    const Result layerRc = nwindowGetVi2Layer(nwindowGetDefault(), &layer);
    const Result displayRc = nwindowGetVi2Display(nwindowGetDefault(), &display);
    if (R_FAILED(layerRc) || !layer) {
        brls::Logger::warning("FRAME_DISPLAY: no VI2 layer rc=0x{:x}", layerRc);
        return;
    }
    if (R_SUCCEEDED(displayRc) && display)
        logDisplayMode(display, "start");

    uint64_t windowStart = nowNs();
    uint64_t lastChange = 0, lastPoll = 0, maxPollGap = 0;
    u64 previous = 0;
    bool havePrevious = false;
    uint32_t bins[kMaxMultiple] = {};
    uint32_t count = 0, offGrid = 0, multiChanges = 0, cadenceChanges = 0, pollErrors = 0;
    double maxMs = 0, sumMs = 0;
    int lastMultiple = 0;

    while (m_running.load()) {
        u64 slots = 0;
        const Result rc = vi2GetLayerAvailability(layer, &slots);
        const uint64_t now = nowNs();
        if (lastPoll)
            maxPollGap = std::max(maxPollGap, now - lastPoll);
        lastPoll = now;
        if (R_FAILED(rc)) {
            pollErrors++;
        } else {
            const u64 reclaimed = havePrevious ? (slots & ~previous) : 0;
            previous = slots;
            havePrevious = true;
            if (reclaimed) {
                if (reclaimed & (reclaimed - 1))
                    multiChanges++; // more than one image change between polls
                if (lastChange) {
                    const double ms = double(now - lastChange) / 1e6;
                    if (ms < 250.0) {
                        const int multiple = std::max(1, int(std::lround(ms / kRefreshMs)));
                        bins[std::min(multiple, kMaxMultiple) - 1]++;
                        if (std::fabs(ms - multiple * kRefreshMs) > kOffGridMs)
                            offGrid++;
                        if (lastMultiple && multiple != lastMultiple)
                            cadenceChanges++;
                        lastMultiple = multiple;
                        count++;
                        sumMs += ms;
                        maxMs = std::max(maxMs, ms);
                    }
                }
                lastChange = now;
            }
        }

        if (now - windowStart >= kReportNs) {
            brls::Logger::info("FRAME_DISPLAY: {:.1f}s changes={} avg_ms={:.2f} max_ms={:.1f} "
                               "refreshes[1,2,3,4,5,6,>=7]=[{},{},{},{},{},{},{}] off_grid={} cadence_changes={} "
                               "multi={} poll_gap_max_ms={:.2f} poll_errors={}",
                double(now - windowStart) / 1e9, count, count ? sumMs / count : 0.0, maxMs, bins[0], bins[1],
                bins[2], bins[3], bins[4], bins[5], bins[6], offGrid, cadenceChanges, multiChanges,
                double(maxPollGap) / 1e6, pollErrors);
            if (display) {
                u32 hz = 0;
                bool vrr = false;
                vi2QueryDisplayRefreshRate(display, &hz, &vrr);
                brls::Logger::info("FRAME_DISPLAY_MODE: refresh={}Hz vrr_active={}", hz, vrr);
            }
            std::fill(std::begin(bins), std::end(bins), 0u);
            count = offGrid = multiChanges = cadenceChanges = pollErrors = 0;
            maxMs = sumMs = 0;
            maxPollGap = 0;
            windowStart = now;
        }
        svcSleepThread(kPollNs);
    }
}

#endif
