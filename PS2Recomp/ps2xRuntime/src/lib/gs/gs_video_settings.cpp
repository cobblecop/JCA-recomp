#include "runtime/gs/gs_video_settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace gs_video
{
    namespace
    {
        constexpr const char *kConfigFileName = "ps2_display.cfg";

        std::once_flag g_loadOnce;

        int clampScale(int v)
        {
            return (v >= 1 && v <= 4) ? v : 1;
        }

        int clampAspect(int v)
        {
            return (v == ASPECT_16_9) ? ASPECT_16_9 : ASPECT_4_3;
        }
    }

    Settings &settings()
    {
        static Settings s;
        return s;
    }

    void loadOnce()
    {
        std::call_once(g_loadOnce, []()
                       {
            // Environment overrides let headless/test runs pick a mode
            // without touching the persisted file.
            Settings &s = settings();
            if (const char *env = std::getenv("PS2_ASPECT"))
            {
                const std::string v(env);
                s.aspect.store((v == "16:9" || v == "16x9" || v == "169")
                                   ? ASPECT_16_9
                                   : ASPECT_4_3,
                               std::memory_order_relaxed);
            }
            if (const char *env = std::getenv("PS2_RENDER_SCALE"))
            {
                s.renderScale.store(clampScale(std::atoi(env)), std::memory_order_relaxed);
            }

            std::ifstream in(kConfigFileName);
            if (!in)
                return;
            std::string key;
            std::string value;
            while (in >> key >> value)
            {
                if (key == "aspect")
                    s.aspect.store(clampAspect(std::atoi(value.c_str())), std::memory_order_relaxed);
                else if (key == "render_scale")
                    s.renderScale.store(clampScale(std::atoi(value.c_str())), std::memory_order_relaxed);
            } });
    }

    void save()
    {
        const Settings &s = settings();
        std::ofstream out(kConfigFileName, std::ios::trunc);
        if (!out)
            return;
        out << "aspect " << s.aspect.load(std::memory_order_relaxed) << "\n"
            << "render_scale " << s.renderScale.load(std::memory_order_relaxed) << "\n";
    }

    float targetAspect()
    {
        return (settings().aspectOr(ASPECT_4_3) == ASPECT_16_9) ? (16.0f / 9.0f)
                                                                 : (4.0f / 3.0f);
    }

    const char *aspectLabel()
    {
        return (settings().aspectOr(ASPECT_4_3) == ASPECT_16_9) ? "16:9" : "4:3";
    }
}
