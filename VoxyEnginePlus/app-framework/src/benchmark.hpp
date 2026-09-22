#pragma once

#include "profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

/// Deterministic, scripted camera flythrough used to compare builds honestly.
///
/// Enable with VOXY_BENCH=<frames>. The camera follows a fixed path (so every run sees the same
/// work), a warm-up period is discarded, then per-frame times and per-pass GPU costs are summarised
/// and the process exits. Optimisation claims should be backed by this, not by eyeballing the HUD.
struct Benchmark
{
    static inline bool active = false;

    // Driven by frame index, not wall time. If the path advanced with the clock, a faster build
    // would render a *different* sequence of camera positions and the two runs would not be
    // comparable. A fixed frame count plus a fixed virtual timestep means every build renders
    // exactly the same frames, so the only thing that differs is how long they took.
    static inline uint32_t measured_frames = 2000;
    static inline uint32_t warmup_frames = 250;
    static inline float virtual_dt = 1.0f / 120.0f;

    static inline uint32_t frame_index = 0;
    static inline std::vector<float> frame_times_ms = {};
    static inline std::map<std::string, std::vector<float>> pass_times_ms = {};
    static inline std::vector<float> gpu_times_ms = {};
    static inline std::vector<float> wait_times_ms = {};

    static inline void Init()
    {
        char const *env = std::getenv("VOXY_BENCH");
        if (env == nullptr)
            return;

        int const requested = std::atoi(env);
        if (requested > 0)
            measured_frames = static_cast<uint32_t>(requested);

        char const *cap = std::getenv("VOXY_CAPTURE");
        if (cap != nullptr)
            capture_dir = cap;

        active = true;
        frame_times_ms.reserve(measured_frames + warmup_frames);
        std::printf("[bench] scripted flythrough: %u frames (+%u warm-up), virtual dt %.4f s\n",
                    measured_frames, warmup_frames, virtual_dt);
    }

    /// Frames at which to dump the rendered image, so two builds can be compared byte for byte.
    /// Set VOXY_CAPTURE=<dir> to enable.
    static inline std::string capture_dir = {};

    static inline bool ShouldCapture()
    {
        if (capture_dir.empty())
            return false;
        // A handful of frames spread across the path: early, mid and late in the orbit.
        // Every 50th frame: dense enough to track a rendering difference across the whole run
        // rather than judging it from a couple of snapshots.
        uint32_t const n = frame_index - warmup_frames;
        return frame_index > warmup_frames && (n % 50 == 0);
    }

    static inline std::string CapturePath()
    {
        return capture_dir + "/frame_" + std::to_string(frame_index) + ".raw";
    }

    /// Virtual time for the camera path, independent of how fast the build actually runs.
    static inline float PathTime()
    {
        return static_cast<float>(frame_index) * virtual_dt;
    }

    /// Fixed camera path over the 65^3 chunk grid: a slow orbit above the canopy, always angled
    /// down and inward so the frame is filled with voxel geometry rather than empty sky. The height
    /// and pitch drift so the run mixes close-up detail, long grazing views and some horizon.
    static inline void SampleCameraPath(float t, float &x, float &y, float &z, float &yaw, float &pitch)
    {
        float const centre = 32.5f;
        float const radius = 22.0f;
        float const angle = t * 0.30f;

        x = centre + std::cos(angle) * radius;
        z = centre + std::sin(angle) * radius;
        y = 44.0f + std::sin(t * 0.55f) * 5.0f;

        // Look inward at the world centre; positive pitch tilts downward in this camera.
        yaw = -(angle * 57.2957795f) + 180.0f;
        pitch = 20.0f + std::sin(t * 0.40f) * 8.0f;
    }

    static inline void Record(float frame_ms)
    {
        if (!active)
            return;

        frame_index++;
        if (frame_index <= warmup_frames)
            return;

        frame_times_ms.push_back(frame_ms);
        gpu_times_ms.push_back(Profiler::gpu_total_raw_ms);
        wait_times_ms.push_back(Profiler::cpu_wait_ms);
        for (auto const &pass : Profiler::passes)
        {
            if (pass.ms_raw > 0.0f)
                pass_times_ms[pass.name].push_back(pass.ms_raw);
        }
    }

    static inline bool Finished()
    {
        return active && frame_index >= (warmup_frames + measured_frames);
    }

    static inline float Percentile(std::vector<float> sorted, float p)
    {
        if (sorted.empty())
            return 0.0f;
        size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
        return sorted[idx];
    }

    static inline void Report()
    {
        if (frame_times_ms.empty())
        {
            std::printf("[bench] no samples collected\n");
            return;
        }

        auto sorted = frame_times_ms;
        std::sort(sorted.begin(), sorted.end());

        double sum = 0.0;
        for (float v : frame_times_ms)
            sum += v;
        double const mean = sum / frame_times_ms.size();

        // "1% low" in the sense gamers use it: the mean of the slowest 1% of frames.
        size_t const low_count = std::max<size_t>(1, sorted.size() / 100);
        double low_sum = 0.0;
        for (size_t i = sorted.size() - low_count; i < sorted.size(); i++)
            low_sum += sorted[i];
        double const low_1 = low_sum / low_count;

        std::printf("\n================ BENCHMARK (%zu frames) ================\n", frame_times_ms.size());
        std::printf("  device            %s\n", Profiler::device_name.c_str());
        std::printf("  mean              %7.3f ms   (%6.1f fps)\n", mean, 1000.0 / mean);
        std::printf("  median            %7.3f ms   (%6.1f fps)\n", Percentile(sorted, 0.50f), 1000.0 / Percentile(sorted, 0.50f));
        std::printf("  p95               %7.3f ms   (%6.1f fps)\n", Percentile(sorted, 0.95f), 1000.0 / Percentile(sorted, 0.95f));
        std::printf("  p99               %7.3f ms   (%6.1f fps)\n", Percentile(sorted, 0.99f), 1000.0 / Percentile(sorted, 0.99f));
        std::printf("  1%% low (mean)     %7.3f ms   (%6.1f fps)\n", low_1, 1000.0 / low_1);
        std::printf("  worst             %7.3f ms\n", sorted.back());
        std::printf("  best              %7.3f ms\n", sorted.front());

        std::printf("\n  GPU pass costs (mean / p99, ms):\n");
        double gpu_total = 0.0;
        for (auto &[name, samples] : pass_times_ms)
        {
            if (samples.empty())
                continue;
            auto s = samples;
            std::sort(s.begin(), s.end());
            double psum = 0.0;
            for (float v : samples)
                psum += v;
            double const pmean = psum / samples.size();
            gpu_total += pmean;
            std::printf("    %-30s %7.3f / %7.3f\n", name.c_str(), pmean, Percentile(s, 0.99f));
        }
        std::printf("    %-30s %7.3f\n", "[gpu total]", gpu_total);
        std::printf("========================================================\n");
        std::fflush(stdout);
    }
};
