#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// Host-side display/render settings. These do not change GS semantics; they
// control how the presented frame is scaled to the window (aspect ratio) and
// at what internal resolution the software rasterizer draws (renderScale).
//
// Values are atomic because they are written from the ImGui debug panel on the
// render thread and read from the EE/game thread by the raster backend.
namespace gs_video
{
    enum AspectRatio : int32_t
    {
        ASPECT_4_3 = 0,  // correct television aspect (pillars on widescreen)
        ASPECT_16_9 = 1, // anamorphic widescreen stretch
    };

    struct Settings
    {
        std::atomic<int32_t> aspect{ASPECT_4_3};
        // Internal render multiplier: 1 = native PS2 resolution (VRAM is the
        // framebuffer), 2..4 = supersampled planes written back on demand.
        std::atomic<int32_t> renderScale{1};

        int32_t aspectOr(int32_t fallback) const
        {
            const int32_t v = aspect.load(std::memory_order_relaxed);
            return (v == ASPECT_4_3 || v == ASPECT_16_9) ? v : fallback;
        }

        int32_t scaleOr(int32_t fallback) const
        {
            const int32_t v = renderScale.load(std::memory_order_relaxed);
            return (v >= 1 && v <= 4) ? v : fallback;
        }
    };

    // Shared process-wide settings instance.
    Settings &settings();

    // Load (once) the persisted settings file next to the working directory.
    // Safe to call multiple times; only the first call reads from disk.
    void loadOnce();

    // Persist current settings to disk (called when the UI changes a value).
    void save();

    // Aspect helpers used by the presentation path.
    float targetAspect();
    const char *aspectLabel();
}
