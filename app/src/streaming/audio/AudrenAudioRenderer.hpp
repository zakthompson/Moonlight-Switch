#ifdef __SWITCH__

#include "IAudioRenderer.hpp"
#include <opus/opus_multistream.h>
#include <switch.h>
#include "AudioHealth.hpp"
#pragma once

// Each wave buffer holds about 10 ms (two 5 ms Opus packets at Sunshine's
// default). audren only takes 4 wave buffers per voice at a time, refilled when
// we call audrvUpdate, so buffer size sets how long a packet-arrival gap the
// voice can ride out: 4 x 10 ms covers the 20-30 ms gaps seen over Wi-Fi.
// 12 buffers (120 ms) leave room for the burst that follows a long gap on a
// rougher link (Switch 1 saw 30-80 ms gaps); latency is bounded by trimming.
#define BUFFER_COUNT 12

class AudrenAudioRenderer : public IAudioRenderer {
  public:
    AudrenAudioRenderer(){};
    ~AudrenAudioRenderer(){};

    int init(int audio_configuration,
             const POPUS_MULTISTREAM_CONFIGURATION opus_config, void* context,
             int ar_flags) override;
    void cleanup() override;
    void decode_and_play_sample(char* sample_data, int sample_length) override;
    int capabilities() override;

  private:
    ssize_t free_wavebuf_index();
    int queued_ms();
    void submit_fill_buffer(int queuedMs);
    void update_trim(int latencyMs);

    OpusMSDecoder* m_decoder = nullptr;
    s16* m_decoded_buffer = nullptr;
    void* mempool_ptr = nullptr;

    AudioDriver m_driver;
    AudioDriverWaveBuf m_wavebufs[BUFFER_COUNT];

    bool m_inited_driver = false;
    int m_channel_count = 0;
    int m_sample_rate = 0;
    int m_frame_samples = 0;   // samples per channel in one Opus packet
    int m_packet_ms = 0;       // duration of one Opus packet
    int m_buffer_samples = 0;  // samples per channel in one wave buffer
    int m_buffer_size = 0;     // bytes in one wave buffer
    int m_buffer_ms = 0;

    // Wave buffer being filled with decoded packets, or -1.
    ssize_t m_fill_index = -1;
    int m_fill_samples = 0;

    // The voice starts playing only once enough audio is queued (pre-roll).
    bool m_primed = false;

    // Latency trimming: drop packets only when latency stayed high for a whole
    // window, which happens after a long stall, not on a single burst.
    u64 m_trim_window_start = 0;
    int m_trim_window_min_ms = 0;
    int m_trim_packets = 0;
    size_t m_dropped_packets = 0;
    AudioHealth m_health;
};

#endif // __SWITCH__
