#ifdef __SWITCH__
#pragma once

#include <Limelight.h>
#include <borealis.hpp>
#include <switch.h>

#include <algorithm>
#include <climits>

// Diagnostics for the audio crackle investigation. The audren renderer calls
// these from the audio decoder thread; one AUDIO_HEALTH line is logged every
// 5 seconds with the renderer's view (underruns, drops, queue depth, decoder
// call gaps) next to the receive thread's view (packet arrival gaps, loss,
// host/client clock drift).
class AudioHealth {
  public:
    void reset(const char* variant, int packetMs, int bufferMs) {
        *this = AudioHealth();
        m_variant = variant;
        m_packet_ms = packetMs > 0 ? packetMs : 5;
        m_buffer_ms = bufferMs;
    }

    void onCall(bool concealment) {
        const u64 now = armGetSystemTick();
        if (m_window_start == 0)
            m_window_start = now;
        if (m_last_call != 0)
            m_max_call_gap_us = std::max(m_max_call_gap_us, ticksToUs(now - m_last_call));
        m_last_call = now;
        m_calls++;
        if (concealment)
            m_concealed++;
        m_pending_max_ms = std::max(m_pending_max_ms, LiGetPendingAudioDuration());
    }

    // queuedMs: audio still waiting to play in audren just before a buffer is
    // added. Zero after the first add means the voice ran dry (an audible gap).
    // countDry: false when the renderer reports underruns itself (onUnderrun).
    void onSubmit(int queuedMs, bool countDry = true) {
        if (countDry && m_submitted > 0 && queuedMs == 0)
            m_underruns++;
        m_submitted++;
        m_submitted_window++;
        m_queued_min_ms = std::min(m_queued_min_ms, queuedMs);
        m_queued_max_ms = std::max(m_queued_max_ms, queuedMs);
    }

    void onUnderrun() { m_underruns++; }
    void onDropTrim() { m_drop_trim++; }
    void onEarlySubmit() { m_early++; }
    void onLatency(int ms) {
        m_latency_min_ms = std::min(m_latency_min_ms, ms);
        m_latency_max_ms = std::max(m_latency_max_ms, ms);
    }
    void setVoiceDrops(u32 drops) { m_voice_drops = drops; }
    // audren's played-sample counter restarts when the voice runs out of
    // buffers, so a decrease means the voice really starved (audible gap).
    void onPlayedCount(u32 played) {
        if (played < m_last_played)
            m_starved++;
        m_last_played = played;
    }
    void onDropPending() { m_drop_pending++; }
    void onDropQueued() { m_drop_queued++; }
    void onDropNoBuffer() { m_drop_nobuf++; }
    void onDropDesync() { m_drop_desync++; }
    void onWaitFrame() { m_waits++; }

    void maybeLog() {
        const u64 now = armGetSystemTick();
        if (m_window_start == 0 || ticksToUs(now - m_window_start) < 5000000)
            return;
        const double seconds = ticksToUs(now - m_window_start) / 1e6;

        AUDIO_ARRIVAL_STATS rx;
        LiGetAudioArrivalStats(&rx);

        // Host audio clock vs ours, from the first report onwards (skipping
        // the start-up backlog). Positive: the host produces audio faster than
        // we receive wall-clock time, so a fixed-size queue grows.
        long long driftPpm = 0;
        if (!m_have_base) {
            m_have_base = rx.totalSeqSpan > 0;
            m_base_span = rx.totalSeqSpan;
            m_base_arrival_us = rx.lastArrivalUs;
        } else if (rx.lastArrivalUs > m_base_arrival_us) {
            const long long wallUs = (long long)(rx.lastArrivalUs - m_base_arrival_us);
            const long long mediaUs =
                (long long)(rx.totalSeqSpan - m_base_span) * m_packet_ms * 1000;
            driftPpm = (mediaUs - wallUs) * 1000000LL / wallUs;
        }

        brls::Logger::info(
            "AUDIO_HEALTH[{}]: {:.1f}s calls={} plc={} submitted={} underruns={} "
            "drop_pending={} drop_queued={} drop_nobuf={} drop_desync={} drop_trim={} "
            "starved={} early={} waits={} pending_max_ms={} queued_ms={}..{} latency_ms={}..{} "
            "buf_ms={} voice_drops={} call_gap_max_ms={:.1f} | "
            "rx_pkts={} rx_missing={} rx_reordered={} rx_gaps>10={} >20={} >40={} "
            "rx_gap_max_ms={:.1f} drift_ppm={}",
            m_variant, seconds, m_calls, m_concealed, m_submitted_window, m_underruns,
            m_drop_pending, m_drop_queued, m_drop_nobuf, m_drop_desync, m_drop_trim,
            m_starved, m_early, m_waits, m_pending_max_ms,
            m_queued_min_ms == INT_MAX ? 0 : m_queued_min_ms, m_queued_max_ms,
            m_latency_min_ms == INT_MAX ? 0 : m_latency_min_ms, m_latency_max_ms,
            m_buffer_ms, m_voice_drops, m_max_call_gap_us / 1000.0, rx.packets,
            rx.seqMissing, rx.reordered, rx.gapsOver10Ms, rx.gapsOver20Ms,
            rx.gapsOver40Ms, rx.maxGapUs / 1000.0, driftPpm);

        m_window_start = now;
        m_calls = m_concealed = m_underruns = m_submitted_window = 0;
        m_drop_pending = m_drop_queued = m_drop_nobuf = m_drop_desync = m_waits = 0;
        m_drop_trim = m_early = m_starved = 0;
        m_latency_min_ms = INT_MAX;
        m_latency_max_ms = 0;
        m_pending_max_ms = 0;
        m_queued_min_ms = INT_MAX;
        m_queued_max_ms = 0;
        m_max_call_gap_us = 0;
        // m_submitted is kept so the underrun check never re-arms mid-stream.
    }

  private:
    static u32 ticksToUs(u64 ticks) { return (u32)(armTicksToNs(ticks) / 1000); }

    const char* m_variant = "";
    int m_packet_ms = 5;
    int m_buffer_ms = 0;
    u64 m_window_start = 0;
    u64 m_last_call = 0;
    u32 m_calls = 0;
    u32 m_concealed = 0;
    u32 m_submitted = 0;
    u32 m_submitted_window = 0;
    u32 m_underruns = 0;
    u32 m_drop_pending = 0;
    u32 m_drop_queued = 0;
    u32 m_drop_nobuf = 0;
    u32 m_drop_desync = 0;
    u32 m_waits = 0;
    u32 m_drop_trim = 0;
    u32 m_early = 0;
    u32 m_starved = 0;
    u32 m_last_played = 0;
    u32 m_voice_drops = 0; // cumulative, from audren
    int m_latency_min_ms = INT_MAX;
    int m_latency_max_ms = 0;
    int m_pending_max_ms = 0;
    int m_queued_min_ms = INT_MAX;
    int m_queued_max_ms = 0;
    u32 m_max_call_gap_us = 0;
    bool m_have_base = false;
    u64 m_base_span = 0;
    u64 m_base_arrival_us = 0;
};

#endif // __SWITCH__
