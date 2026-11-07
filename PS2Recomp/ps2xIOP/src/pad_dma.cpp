#include "ps2x/iop/pad_dma.h"

#include <algorithm>
#include <mutex>

namespace ps2x::iop
{
    namespace
    {
        std::mutex g_padDmaMutex;
        std::vector<PadDmaArea> g_padDmaAreas;

        void removeAreaLocked(int port, int slot)
        {
            g_padDmaAreas.erase(std::remove_if(g_padDmaAreas.begin(), g_padDmaAreas.end(),
                                               [port, slot](const PadDmaArea &entry)
                                               {
                                                   return entry.port == port && entry.slot == slot;
                                               }),
                                g_padDmaAreas.end());
        }
    }

    void padDmaSetArea(int port, int slot, uint32_t area)
    {
        std::lock_guard<std::mutex> lock(g_padDmaMutex);
        removeAreaLocked(port, slot);
        if (area != 0u)
        {
            g_padDmaAreas.push_back(PadDmaArea{port, slot, area});
        }
    }

    void padDmaClearArea(int port, int slot)
    {
        std::lock_guard<std::mutex> lock(g_padDmaMutex);
        removeAreaLocked(port, slot);
    }

    void padDmaReset()
    {
        std::lock_guard<std::mutex> lock(g_padDmaMutex);
        g_padDmaAreas.clear();
    }

    std::vector<PadDmaArea> padDmaAreas()
    {
        std::lock_guard<std::mutex> lock(g_padDmaMutex);
        return g_padDmaAreas;
    }
}
