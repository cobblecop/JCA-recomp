#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Records the guest instruction that performed the most recent MMIO store.
// The GS processes GIF packets and transfers synchronously from inside that
// store (or from the DMA kick it triggers), so a GS-side diagnostic can name
// the EE routine responsible without threading a context through the bus.
namespace ps2_guest_debug
{
    inline uint32_t last_store_pc = 0;
    inline uint32_t last_store_ra = 0;
    inline uint32_t last_store_addr = 0;

    // Live RDRAM base, refreshed by every guest store (see
    // ps2TraceGuestWrite) so GS-side diagnostics can inspect EE memory.
    inline uint8_t *rdram = nullptr;

    // PS2_WRITEWATCH=0xLO:0xHI arms a guest write watchpoint over a physical
    // RDRAM range, so the routine filling a buffer can be named directly.
    struct WriteWatch
    {
        uint32_t lo = 0;
        uint32_t hi = 0;
        uint8_t pattern = 0;   // when set, match stores of this repeated byte
        bool byPattern = false;
        bool enabled = false;
        uint32_t hits = 0;
        uint32_t reported = 0;
        // One entry per distinct (pc,ra) that produced a matching store, so a
        // fill is never hidden by the per-hit print cap.
        struct Writer
        {
            uint32_t pc = 0;
            uint32_t ra = 0;
            uint32_t count = 0;
            uint32_t firstAddr = 0;
        };
        static constexpr uint32_t kMaxWriters = 24u;
        Writer writers[kMaxWriters]{};
        uint32_t writerCount = 0;
    };
    inline WriteWatch watch;

    inline uint32_t parseHex(const char *&s)
    {
        uint32_t v = 0;
        bool any = false;
        while (*s != '\0' && *s != ':')
        {
            const char c = *s;
            uint32_t d;
            if (c >= '0' && c <= '9')
                d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = static_cast<uint32_t>(c - 'A' + 10);
            else
                break;
            v = (v << 4) | d;
            any = true;
            ++s;
        }
        return any ? v : 0u;
    }

    inline WriteWatch &writeWatch()
    {
        static WriteWatch initialized = []
        {
            WriteWatch w;
            if (const char *env = std::getenv("PS2_WRITEWATCH"); env && env[0] != '\0')
            {
                const char *p = env;
                while (*p == ' ')
                    ++p;
                const uint32_t lo = parseHex(p);
                if (lo != 0u && lo < 0x100u)
                {
                    // A small single value arms a value watchpoint instead of a
                    // range: catches a fill loop without knowing its address.
                    w.pattern = static_cast<uint8_t>(lo);
                    w.byPattern = true;
                    w.enabled = true;
                }
                else if (*p == ':')
                {
                    ++p;
                    const uint32_t hi = parseHex(p);
                    if (hi > lo)
                    {
                        w.lo = lo;
                        w.hi = hi;
                        w.enabled = true;
                    }
                }
            }
            return w;
        }();
        return initialized;
    }

    // Called from ps2TraceGuestWrite for every guest store; must stay cheap.
    inline void onGuestWrite(uint32_t guestAddr, uint32_t size, uint64_t valueLo,
                             uint64_t valueHi, uint32_t pc, uint32_t ra, uint32_t sp)
    {
        WriteWatch &w = writeWatch();
        if (!w.enabled)
            return;
        if (w.byPattern)
        {
            uint64_t all = 0x0101010101010101ull * w.pattern;
            bool match = false;
            if (size == 1u)
                match = (valueLo & 0xFFull) == static_cast<uint64_t>(w.pattern);
            else if (size == 2u)
                match = (valueLo & 0xFFFFull) == (all & 0xFFFFull);
            else if (size == 4u)
                match = (valueLo & 0xFFFFFFFFull) == (all & 0xFFFFFFFFull);
            else
                match = valueLo == all || valueHi == all;
            if (!match)
                return;
            if (guestAddr >= 0x20000000u)
                return;
        }
        else if (guestAddr + size <= w.lo || guestAddr >= w.hi)
        {
            return;
        }
        ++w.hits;
        if ((w.hits % 2000u) == 0u)
        {
            std::fprintf(stderr, "[wwatch] at %u hits, writers:", w.hits);
            for (uint32_t i = 0; i < w.writerCount; ++i)
                std::fprintf(stderr, " %08x/%08x=%u(first %08x)", w.writers[i].pc,
                             w.writers[i].ra, w.writers[i].count, w.writers[i].firstAddr);
            std::fprintf(stderr, "\n");
        }
        // Tally per writer first (cheap, unbounded), then print the first few
        // hits of each new writer only.
        for (uint32_t i = 0; i < w.writerCount; ++i)
        {
            if (w.writers[i].pc == pc && w.writers[i].ra == ra)
            {
                ++w.writers[i].count;
                return;
            }
        }
        if (w.writerCount < WriteWatch::kMaxWriters)
        {
            WriteWatch::Writer &nw = w.writers[w.writerCount++];
            nw.pc = pc;
            nw.ra = ra;
            nw.count = 1u;
            nw.firstAddr = guestAddr;
            std::fprintf(stderr,
                         "[wwatch] newwriter pc=%08x ra=%08x firstAddr=%08x size=%u value=%016llx:%016llx sp=%08x\n",
                         pc, ra, guestAddr, size,
                         static_cast<unsigned long long>(valueHi),
                         static_cast<unsigned long long>(valueLo), sp);
        }
        if (w.reported >= 60u)
            return;
        ++w.reported;
        std::fprintf(stderr,
                     "[wwatch] hit=%u pc=%08x ra=%08x addr=%08x size=%u value=%016llx:%016llx sp=%08x\n",
                     w.hits, pc, ra, guestAddr, size,
                     static_cast<unsigned long long>(valueHi),
                     static_cast<unsigned long long>(valueLo), sp);
    }
}
