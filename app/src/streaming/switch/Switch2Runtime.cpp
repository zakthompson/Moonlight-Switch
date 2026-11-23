#ifdef __SWITCH__
// fopencookie() is a GNU extension in newlib.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "Switch2Runtime.hpp"

#include <borealis.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <switch.h>
#include <thread>
#include <utility>

namespace
{
// Bytes queued beyond this are dropped and counted rather than blocking the caller.
constexpr size_t kMaxPending = 1024 * 1024;
// Coalesce bursts of lines into one SD-card write.
constexpr auto kWriteInterval = std::chrono::milliseconds(20);

std::mutex g_mutex;
std::condition_variable g_wake;
std::string g_pending;
size_t g_dropped   = 0;
bool g_stopped     = false;
FILE* g_file       = nullptr;
std::thread* g_writer = nullptr;

void writeLoop()
{
    std::string batch;
    for (;;)
    {
        size_t dropped = 0;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_pending.empty() || g_stopped; });
            if (g_pending.empty())
                return;
            batch.swap(g_pending);
            dropped = std::exchange(g_dropped, 0);
        }
        if (dropped)
            fprintf(g_file, "[async log: dropped %zu bytes]\n", dropped);
        fwrite(batch.data(), 1, batch.size(), g_file);
        fflush(g_file);
        batch.clear();
        std::this_thread::sleep_for(kWriteInterval);
    }
}

ssize_t queueWrite(void*, const char* data, size_t size)
{
    bool wake;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stopped)
        {
            // The writer has exited; late messages (from exit paths) go straight to the file.
            fwrite(data, 1, size, g_file);
            fflush(g_file);
            return ssize_t(size);
        }
        wake = g_pending.empty();
        if (g_pending.size() + size > kMaxPending)
            g_dropped += size;
        else
            g_pending.append(data, size);
    }
    if (wake)
        g_wake.notify_one();
    return ssize_t(size);
}

void stopWriter()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stopped = true;
    }
    g_wake.notify_one();
    if (g_writer && g_writer->joinable())
        g_writer->join();
}
} // namespace

FILE* switch2OpenAsyncLog(FILE* file)
{
    g_file = file;
    g_pending.reserve(64 * 1024);
    cookie_io_functions_t io { nullptr, queueWrite, nullptr, nullptr };
    FILE* queued = fopencookie(nullptr, "w", io);
    if (!queued)
        return file;
    g_writer = new std::thread(writeLoop);
    std::atexit(stopWriter);
    return queued;
}

#ifdef __SWITCH2__
// Set by Borealis's userAppInit.
extern "C" u32 brls_switch2_udp_rx_buf_size;
extern "C" int brls_switch2_udp_rx_probe_errno[3];
extern "C" const u32 brls_switch2_udp_rx_sizes[3];

void switch2LogSocketConfiguration()
{
    const AppletType type = appletGetAppletType();
    for (int i = 0; i < 3; i++)
    {
        if (brls_switch2_udp_rx_probe_errno[i] == -1)
            brls::Logger::warning("SWITCH2_NET: udp_rx_buf_size=0x{:x} rejected by socketInitialize",
                brls_switch2_udp_rx_sizes[i]);
        else if (brls_switch2_udp_rx_probe_errno[i])
            brls::Logger::warning("SWITCH2_NET: udp_rx_buf_size=0x{:x} rejected, UDP socket() errno {}",
                brls_switch2_udp_rx_sizes[i], brls_switch2_udp_rx_probe_errno[i]);
    }
    if (brls_switch2_udp_rx_buf_size)
        brls::Logger::info("SWITCH2_NET: applet type {}, socket driver udp_rx_buf_size=0x{:x}", int(type),
            brls_switch2_udp_rx_buf_size);
    else
        brls::Logger::error("SWITCH2_NET: applet type {}, streaming socket configuration not applied "
                            "(not an application, or every size was rejected)", int(type));
}
#endif // __SWITCH2__
#endif // __SWITCH__
