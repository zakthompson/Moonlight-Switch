#pragma once
#ifdef __SWITCH2__

#include <atomic>
#include <thread>

// Diagnostics for display-timing alignment. While a stream runs, a thread polls
// the window's VI2 layer availability bitmask every 0.5 ms. VI2 reclaims an
// image slot when a newer image replaces it on screen, so each newly available
// slot timestamps an on-screen image change. One FRAME_DISPLAY line every 5 s
// reports those intervals as multiples of the 120 Hz refresh (8.33 ms), how
// many fall off that grid (expected only with VRR), and the display's nominal
// refresh rate and VRR state from VI2.
class Switch2DisplayTiming {
  public:
    static Switch2DisplayTiming& instance();
    void start();
    void stop();

  private:
    void run();
    std::thread m_thread;
    std::atomic<bool> m_running { false };
};

#endif
