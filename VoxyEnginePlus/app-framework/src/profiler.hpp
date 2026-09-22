#pragma once

#include <array>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <vector>

/// Lightweight global sink for GPU/CPU frame timings.
/// The renderer fills this in every frame; the UI layer reads it.
struct Profiler
{
    struct Entry
    {
        std::string name;
        float ms = 0.0f;     // smoothed
        float ms_raw = 0.0f; // last resolved sample
    };

    // Off by default so the game renders exactly what it rendered before; F3 toggles it.
    static inline bool enabled = false;
    static inline std::vector<Entry> passes = {};
    static inline float gpu_total_ms = 0.0f;
    static inline float gpu_total_raw_ms = 0.0f; // unsmoothed, for spike attribution
    static inline float cpu_frame_ms = 0.0f;
    static inline float cpu_wait_ms = 0.0f;
    static inline float fps = 0.0f;
    static inline std::string device_name = "unknown";

    // 1% low tracking over a rolling window of frame times
    static inline std::array<float, 256> history = {};
    static inline size_t history_head = 0;
    static inline float low_1_percent_ms = 0.0f;

    static inline void PushFrameTime(float ms)
    {
        history[history_head] = ms;
        history_head = (history_head + 1) % history.size();

        // 1% low == the worst ~1% of the window; with 256 samples that is the 3rd worst.
        float w0 = 0, w1 = 0, w2 = 0;
        for (float v : history)
        {
            if (v > w0)
            {
                w2 = w1;
                w1 = w0;
                w0 = v;
            }
            else if (v > w1)
            {
                w2 = w1;
                w1 = v;
            }
            else if (v > w2)
            {
                w2 = v;
            }
        }
        low_1_percent_ms = w2;
    }

    static inline void Smooth(Entry &e, float sample)
    {
        e.ms_raw = sample;
        e.ms = e.ms * 0.92f + sample * 0.08f;
    }

    /// Periodic console dump, for headless measurement runs. Enable with VOXY_PROFILE_LOG=1.
    static inline bool console_log = []
    {
        char const *v = std::getenv("VOXY_PROFILE_LOG");
        return v != nullptr && v[0] == '1';
    }();

    static inline double last_log_time = 0.0;

    static inline void MaybeLog(double now_seconds)
    {
        if (!console_log || now_seconds - last_log_time < 1.0)
            return;
        last_log_time = now_seconds;

        std::printf("\n[t=%6.1fs] %.1f fps | cpu %.2f ms | gpu %.2f ms | 1%% low %.2f ms | present wait %.2f ms\n",
                    now_seconds, fps, cpu_frame_ms, gpu_total_ms, low_1_percent_ms, cpu_wait_ms);
        for (auto const &pass : passes)
        {
            std::printf("    %-26s %7.3f ms  (%4.1f%%)\n", pass.name.c_str(), pass.ms,
                        gpu_total_ms > 0.0f ? 100.0f * pass.ms / gpu_total_ms : 0.0f);
        }
    }
};
