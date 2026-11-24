#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// Diagnostics for uneven video pacing. Records the interval between
// consecutive frames at each pipeline stage, so one FRAME_PACING line every
// 5 s shows where an even host cadence turns uneven:
//   host     host capture timestamps (RTP clock): the host's own pacing
//   sent     capture + host processing latency: when the host sent it
//   recv     first packet of each frame arriving: network delivery
//   decoded  hardware decoder output pushed to the frame queue
//   drawn    UI thread taking the frame for the next present
// Each stage gets a histogram of intervals (ms bins <4, 4-12, 12-21, 21-29,
// 29-38, >=38), the max interval and the mean absolute deviation from the
// host's mean interval. Thread-safe; all times are microseconds on one clock
// except host, which only needs to be self-consistent.
class FramePacing {
  public:
    enum Stage { Host, Sent, Receive, Decoded, Drawn, StageCount };
    static constexpr int kBins = 6;

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stages = {};
        m_latency = {};
        m_host_latency = {};
        m_depth = {};
        m_pending = {};
    }

    void record(Stage stage, uint64_t us) {
        std::lock_guard<std::mutex> lock(m_mutex);
        recordLocked(stage, us);
    }

    // Called once per received frame: host stamp, host processing latency
    // (capture to send, in 0.1 ms units; 0 when the host does not report it),
    // arrival time.
    void received(uint64_t hostUs, uint32_t hostLatencyTenthsMs, uint64_t receiveUs) {
        std::lock_guard<std::mutex> lock(m_mutex);
        recordLocked(Host, hostUs);
        if (hostLatencyTenthsMs) {
            recordLocked(Sent, hostUs + uint64_t(hostLatencyTenthsMs) * 100);
            m_host_latency.sum += hostLatencyTenthsMs * 100ull;
            m_host_latency.count++;
            m_host_latency.max = std::max<uint64_t>(m_host_latency.max, hostLatencyTenthsMs * 100ull);
            m_host_latency.min = std::min<uint64_t>(m_host_latency.min, hostLatencyTenthsMs * 100ull);
        }
        recordLocked(Receive, receiveUs);
        m_pending[m_pending_next] = { hostUs, receiveUs };
        m_pending_next = (m_pending_next + 1) % m_pending.size();
    }

    // Called when the UI thread takes a frame. hostUs identifies the frame;
    // records arrival-to-draw latency and the queue depth left behind.
    void drawn(uint64_t hostUs, uint64_t nowUs, size_t queueDepth) {
        std::lock_guard<std::mutex> lock(m_mutex);
        recordLocked(Drawn, nowUs);
        for (const auto& item : m_pending) {
            if (item.first == hostUs && item.second && nowUs >= item.second) {
                const uint64_t latency = nowUs - item.second;
                m_latency.sum += latency;
                m_latency.count++;
                m_latency.max = std::max(m_latency.max, latency);
                m_latency.min = std::min(m_latency.min, latency);
                break;
            }
        }
        m_depth.sum += queueDepth;
        m_depth.count++;
        m_depth.max = std::max<uint64_t>(m_depth.max, queueDepth);
    }

    // Returns the report and starts a new window.
    std::string report(float seconds) {
        std::lock_guard<std::mutex> lock(m_mutex);
        static const char* names[StageCount] = { "host", "sent", "recv", "decoded", "drawn" };
        std::string out;
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "FRAME_PACING: %.1fs bins_ms=<4,4-12,12-21,21-29,29-38,>=38", seconds);
        out += buffer;
        const auto& host = m_stages[Host];
        const double meanUs = host.count ? double(host.sum) / host.count : 0;
        for (int stage = 0; stage < StageCount; stage++) {
            auto& s = m_stages[stage];
            const double mad = s.count && meanUs > 0 ? s.absDevFrom(meanUs) / s.count / 1000.0 : 0;
            std::snprintf(buffer, sizeof(buffer), " | %s n=%u [%u,%u,%u,%u,%u,%u] max=%.1f mad=%.2f", names[stage],
                s.count, s.bins[0], s.bins[1], s.bins[2], s.bins[3], s.bins[4], s.bins[5], s.max / 1000.0, mad);
            out += buffer;
            s.resetWindow();
        }
        std::snprintf(buffer, sizeof(buffer), " | latency_ms avg=%.1f min=%.1f max=%.1f | depth avg=%.2f max=%u",
            m_latency.count ? double(m_latency.sum) / m_latency.count / 1000.0 : 0,
            m_latency.count ? m_latency.min / 1000.0 : 0, m_latency.max / 1000.0,
            m_depth.count ? double(m_depth.sum) / m_depth.count : 0, unsigned(m_depth.max));
        out += buffer;
        std::snprintf(buffer, sizeof(buffer), " | host_latency_ms avg=%.1f min=%.1f max=%.1f",
            m_host_latency.count ? double(m_host_latency.sum) / m_host_latency.count / 1000.0 : 0,
            m_host_latency.count ? m_host_latency.min / 1000.0 : 0, m_host_latency.max / 1000.0);
        out += buffer;
        m_host_latency = {};
        m_latency = {};
        m_depth = {};
        return out;
    }

  private:
    struct StageStats {
        uint64_t last = 0;
        uint32_t count = 0;
        uint64_t sum = 0;
        uint64_t max = 0;
        uint32_t bins[kBins] = {};
        // Intervals are kept to compute deviation from the host mean at report
        // time; 5 s at up to 120 fps fits.
        std::array<uint32_t, 1024> intervals = {};

        void add(uint64_t interval) {
            if (count < intervals.size())
                intervals[count] = uint32_t(std::min<uint64_t>(interval, UINT32_MAX));
            count++;
            sum += interval;
            max = std::max(max, interval);
            const uint64_t ms = interval / 1000;
            const int bin = ms < 4 ? 0 : ms < 12 ? 1 : ms < 21 ? 2 : ms < 29 ? 3 : ms < 38 ? 4 : 5;
            bins[bin]++;
        }
        double absDevFrom(double mean) const {
            double total = 0;
            const uint32_t n = std::min<uint32_t>(count, intervals.size());
            for (uint32_t i = 0; i < n; i++)
                total += intervals[i] > mean ? intervals[i] - mean : mean - intervals[i];
            return count > n ? total * count / n : total;
        }
        void resetWindow() {
            const uint64_t keep = last;
            *this = StageStats();
            last = keep;
        }
    };
    struct Summary {
        uint64_t sum = 0;
        uint32_t count = 0;
        uint64_t max = 0;
        uint64_t min = UINT64_MAX;
    };

    void recordLocked(Stage stage, uint64_t us) {
        auto& s = m_stages[stage];
        // Ignore the first frame and clock steps (stream restarts, sleep).
        if (s.last && us > s.last && us - s.last < 1000000)
            s.add(us - s.last);
        s.last = us;
    }

    std::mutex m_mutex;
    std::array<StageStats, StageCount> m_stages = {};
    Summary m_latency;
    Summary m_host_latency;
    Summary m_depth;
    std::array<std::pair<uint64_t, uint64_t>, 32> m_pending = {};
    size_t m_pending_next = 0;
};

inline FramePacing& framePacing() {
    static FramePacing instance;
    return instance;
}
