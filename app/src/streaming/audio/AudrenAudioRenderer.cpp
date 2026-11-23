#ifdef __SWITCH__

#include "AudrenAudioRenderer.hpp"
#include <Settings.hpp>
#include <borealis.hpp>
#include <algorithm>
#include <climits>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t m_sink_channels[] = {0, 1};

namespace {

// Target audio per wave buffer (rounded to whole Opus packets).
constexpr int BUFFER_MS = 10;
// Audio queued in audren before the voice starts. Absorbs the packet-arrival
// jitter seen over Wi-Fi (gaps up to ~30 ms) without much delay.
constexpr int PREROLL_MS = 30;
// If total latency (moonlight-common-c queue + audren queue + the buffer being
// filled) never drops below TRIM_THRESHOLD_MS for a whole window, drop packets
// to bring it back to TRIM_TARGET_MS. Host and client clocks were measured to
// agree within noise, so this only acts after a stall leaves a backlog.
constexpr int TRIM_WINDOW_MS = 1000;
constexpr int TRIM_THRESHOLD_MS = 60;
constexpr int TRIM_TARGET_MS = 40;

void applyVolume(s16* buffer, size_t sampleCount, int volume) {
    if (volume == 100)
        return;

    for (size_t i = 0; i < sampleCount; i++) {
        const int scaled = (static_cast<int>(buffer[i]) * volume) / 100;
        buffer[i] = static_cast<s16>(std::min<int>(SHRT_MAX, std::max<int>(SHRT_MIN, scaled)));
    }
}

} // namespace

static const AudioRendererConfig m_ar_config = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 24,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

int AudrenAudioRenderer::init(int audio_configuration,
                              const POPUS_MULTISTREAM_CONFIGURATION opus_config,
                              void* context, int ar_flags) {
    m_channel_count = opus_config->channelCount;
    m_sample_rate = opus_config->sampleRate;
    m_frame_samples = opus_config->samplesPerFrame > 0
                          ? opus_config->samplesPerFrame
                          : AUDREN_SAMPLES_PER_FRAME_48KHZ;
    m_packet_ms = std::max(1, m_frame_samples * 1000 / m_sample_rate);
    const int packetsPerBuffer = std::max(1, BUFFER_MS / m_packet_ms);
    m_buffer_samples = m_frame_samples * packetsPerBuffer;
    m_buffer_size = m_buffer_samples * m_channel_count * sizeof(s16);
    m_buffer_ms = m_buffer_samples * 1000 / m_sample_rate;
    m_fill_index = -1;
    m_fill_samples = 0;
    m_primed = false;
    m_trim_window_start = 0;
    m_trim_window_min_ms = INT_MAX;
    m_trim_packets = 0;
    m_dropped_packets = 0;
    m_health.reset("fix", m_packet_ms, m_buffer_ms);

    brls::Logger::info("Audren: Init with channels: {}, sample rate: {}, frame: {} samples",
                       m_channel_count, m_sample_rate, m_frame_samples);

    m_decoded_buffer = (s16*)malloc(m_frame_samples * m_channel_count * sizeof(s16));
    if (!m_decoded_buffer)
        return -1;

    int error;
    m_decoder = opus_multistream_decoder_create(
        opus_config->sampleRate, opus_config->channelCount,
        opus_config->streams, opus_config->coupledStreams, opus_config->mapping,
        &error);
    if (!m_decoder || error != OPUS_OK) {
        cleanup();
        return -1;
    }

    memset(&m_driver, 0, sizeof(m_driver));
    memset(m_wavebufs, 0, sizeof(m_wavebufs));

    int mempool_size =
        (m_buffer_size * BUFFER_COUNT + (AUDREN_MEMPOOL_ALIGNMENT - 1)) &
        ~(AUDREN_MEMPOOL_ALIGNMENT - 1);
    mempool_ptr = memalign(AUDREN_MEMPOOL_ALIGNMENT, mempool_size);

    // moonlight-common-c does not call cleanup() when init() fails, so free
    // what was allocated here on every failure path.
    if (!mempool_ptr) {
        brls::Logger::error("Audren: mempool alloc failed");
        cleanup();
        return -1;
    }

    Result rc = audrenInitialize(&m_ar_config);
    if (R_FAILED(rc)) {
        brls::Logger::error("Audren: audrenInitialize: {:x}", rc);
        cleanup();
        return -1;
    }

    rc = audrvCreate(&m_driver, &m_ar_config, m_channel_count);
    if (R_FAILED(rc)) {
        brls::Logger::error("Audren: audrvCreate: {:x}", rc);
        audrenExit();
        cleanup();
        return -1;
    }
    m_inited_driver = true;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        m_wavebufs[i].data_raw = mempool_ptr;
        m_wavebufs[i].size = mempool_size;
        m_wavebufs[i].start_sample_offset = i * m_buffer_samples;
        m_wavebufs[i].end_sample_offset =
            m_wavebufs[i].start_sample_offset + m_buffer_samples;
    }

    int mpid = audrvMemPoolAdd(&m_driver, mempool_ptr, mempool_size);
    audrvMemPoolAttach(&m_driver, mpid);

    audrvDeviceSinkAdd(&m_driver, AUDREN_DEFAULT_DEVICE_NAME, m_channel_count,
                       m_sink_channels);

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        brls::Logger::error("Audren: audrenStartAudioRenderer: {:x}", rc);
        cleanup();
        return -1;
    }

    audrvVoiceInit(&m_driver, 0, m_channel_count, PcmFormat_Int16,
                   m_sample_rate);
    audrvVoiceSetDestinationMix(&m_driver, 0, AUDREN_FINAL_MIX_ID);

    for (int i = 0; i < m_channel_count; i++) {
        for (int j = 0; j < m_channel_count; j++) {
            audrvVoiceSetMixFactor(&m_driver, 0, i == j ? 1.0f : 0.0f, i, j);
        }
    }

    // Started but paused: queued buffers are kept until the pre-roll is met.
    audrvVoiceStart(&m_driver, 0);
    audrvVoiceSetPaused(&m_driver, 0, true);
    audrvUpdate(&m_driver);

    m_inited_driver = true;

    brls::Logger::info("Audren: Init done!");

    return DR_OK;
}

void AudrenAudioRenderer::cleanup() {
    brls::Logger::info("Audren: Cleanup... dropped {} late packets",
                       m_dropped_packets);

    // Stop and close the driver before freeing the memory pool it still uses.
    if (m_inited_driver) {
        m_inited_driver = false;
        audrvVoiceStop(&m_driver, 0);
        audrvClose(&m_driver);
        audrenExit();
    }

    if (m_decoder) {
        opus_multistream_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }

    if (m_decoded_buffer) {
        free(m_decoded_buffer);
        m_decoded_buffer = nullptr;
    }

    if (mempool_ptr) {
        free(mempool_ptr);
        mempool_ptr = nullptr;
    }

    brls::Logger::info("Audren: Cleanup done!");
}

// Audio handed to audren and not played yet. Counted from the wave buffer
// states rather than the voice's played-sample counter, which restarts when
// the voice runs dry and restarts.
int AudrenAudioRenderer::queued_ms() {
    int samples = 0;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (m_wavebufs[i].state == AudioDriverWaveBufState_Waiting ||
            m_wavebufs[i].state == AudioDriverWaveBufState_Queued ||
            m_wavebufs[i].state == AudioDriverWaveBufState_Playing)
            samples += m_wavebufs[i].end_sample_offset - m_wavebufs[i].start_sample_offset;
    }
    return samples * 1000 / m_sample_rate;
}

void AudrenAudioRenderer::submit_fill_buffer(int queuedMs) {
    AudioDriverWaveBuf* wavebuf = &m_wavebufs[m_fill_index];
    wavebuf->end_sample_offset = wavebuf->start_sample_offset + m_fill_samples;
    audrvVoiceAddWaveBuf(&m_driver, 0, wavebuf);
    m_health.onSubmit(queuedMs, false);
    m_fill_index = -1;
    m_fill_samples = 0;
}

void AudrenAudioRenderer::update_trim(int latencyMs) {
    const u64 now = armGetSystemTick();
    if (!m_primed) {
        // Only judge latency while playing; restart the window afterwards.
        m_trim_window_start = 0;
        return;
    }
    if (m_trim_window_start == 0) {
        m_trim_window_start = now;
        m_trim_window_min_ms = INT_MAX;
    }
    m_trim_window_min_ms = std::min(m_trim_window_min_ms, latencyMs);
    if (armTicksToNs(now - m_trim_window_start) / 1000000 >= (u64)TRIM_WINDOW_MS) {
        if (m_trim_window_min_ms > TRIM_THRESHOLD_MS)
            m_trim_packets = (m_trim_window_min_ms - TRIM_TARGET_MS) / m_packet_ms;
        m_trim_window_start = now;
        m_trim_window_min_ms = INT_MAX;
    }
}

void AudrenAudioRenderer::decode_and_play_sample(char* data, int length) {
    if (!m_inited_driver || !m_decoder || !m_decoded_buffer) {
        brls::Logger::error("Audren: Invalid call of decode_and_play_sample");
        return;
    }

    m_health.onCall(data == nullptr);
    m_health.maybeLog();

    // Always decode, even packets we drop, so the Opus decoder state stays
    // continuous. A NULL packet asks Opus for packet loss concealment.
    const int decoded = opus_multistream_decode(
        m_decoder, (const unsigned char*)data, data ? length : 0,
        m_decoded_buffer, m_frame_samples, 0);
    if (decoded <= 0)
        return;

    // Refresh the wave buffer states (and feed audren's 4-entry queue).
    audrvUpdate(&m_driver);
    m_health.setVoiceDrops(audrvVoiceGetVoiceDropsCount(&m_driver, 0));
    m_health.onPlayedCount(audrvVoiceGetPlayedSampleCount(&m_driver, 0));

    // An empty queue here is often just a buffer finishing on a frame boundary
    // as this packet arrives; it is submitted right away below rather than
    // pausing to rebuild the pre-roll, which would itself leave a gap.
    int queued = queued_ms();
    if (m_primed && queued == 0)
        m_health.onUnderrun();

    const int latency = LiGetPendingAudioDuration() + queued +
                        m_fill_samples * 1000 / m_sample_rate;
    m_health.onLatency(latency);
    update_trim(latency);
    if (m_trim_packets > 0) {
        m_trim_packets--;
        m_health.onDropTrim();
        m_dropped_packets++;
        return;
    }

    if (m_fill_index < 0) {
        m_fill_index = free_wavebuf_index();
        if (m_fill_index < 0) {
            m_health.onDropNoBuffer();
            m_dropped_packets++;
            return;
        }
        m_fill_samples = 0;
    }

    const int samples = std::min(decoded, m_buffer_samples - m_fill_samples);
    applyVolume(m_decoded_buffer, static_cast<size_t>(samples) * m_channel_count,
                Settings::instance().get_volume());
    const size_t bytes = static_cast<size_t>(samples) * m_channel_count * sizeof(s16);
    char* dst = static_cast<char*>(mempool_ptr) + m_fill_index * m_buffer_size +
                static_cast<size_t>(m_fill_samples) * m_channel_count * sizeof(s16);
    memcpy(dst, m_decoded_buffer, bytes);
    armDCacheFlush(dst, bytes);
    m_fill_samples += samples;

    // Submit a full buffer, or a partial one if the voice is about to run dry.
    const bool full = m_fill_samples + m_frame_samples > m_buffer_samples;
    const bool aboutToRunDry = m_primed && queued <= m_packet_ms;
    if (full || aboutToRunDry) {
        if (!full)
            m_health.onEarlySubmit();
        submit_fill_buffer(queued);
        queued = queued_ms();
    }

    if (!m_primed && queued >= PREROLL_MS) {
        m_primed = true;
        audrvVoiceSetPaused(&m_driver, 0, false);
    }

    audrvUpdate(&m_driver);
}

int AudrenAudioRenderer::capabilities() {
    return 0;
}

ssize_t AudrenAudioRenderer::free_wavebuf_index() {
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (m_wavebufs[i].state == AudioDriverWaveBufState_Free ||
            m_wavebufs[i].state == AudioDriverWaveBufState_Done) {
            return i;
        }
    }
    return -1;
}

#endif //__SWITCH__
