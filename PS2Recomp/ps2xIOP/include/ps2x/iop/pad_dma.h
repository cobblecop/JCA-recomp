#pragma once

#include <cstdint>
#include <vector>

namespace ps2x::iop
{
    // A guest pad area registered by libpad through the PADMAN open command.
    // The area is 0x100 bytes: two 0x80-byte pad status frames that PADMAN
    // normally keeps up to date through DMA. The EE side fills them.
    struct PadDmaArea
    {
        int port = 0;
        int slot = 0;
        uint32_t area = 0u;
    };

    void padDmaSetArea(int port, int slot, uint32_t area);
    void padDmaClearArea(int port, int slot);
    void padDmaReset();
    [[nodiscard]] std::vector<PadDmaArea> padDmaAreas();
}
