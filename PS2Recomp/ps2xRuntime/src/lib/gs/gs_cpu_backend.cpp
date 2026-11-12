#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_video_settings.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_guest_debug.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

// Defined in gs_frontend.cpp: frame index and `first:last` trace windows.
namespace gs_debug
{
    uint32_t frame();
    bool window(const char *name);
}

using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    // Byte-exact 5551 -> 8888 expansion. This runs for every sampled texel of
    // every 16-bit texture, so it is table driven: replicating the top bits
    // (so full-scale expands to 255 instead of 248) costs enough shifts per
    // call to show up in profiles.
    struct Rgba5551Table
    {
        std::array<u32, 65536> values{};
        Rgba5551Table()
        {
            for (u32 c = 0; c < 65536u; ++c)
            {
                const u32 r5 = (c >> 0) & 0x1Fu;
                const u32 g5 = (c >> 5) & 0x1Fu;
                const u32 b5 = (c >> 10) & 0x1Fu;
                const u32 r = (r5 << 3) | (r5 >> 2);
                const u32 g = (g5 << 3) | (g5 >> 2);
                const u32 b = (b5 << 3) | (b5 >> 2);
                const u32 a = ((c >> 15) & 0x01u) ? 0xFFu : 0x00u;
                values[c] = r | (g << 8) | (b << 16) | (a << 24);
            }
        }
    };

    inline u32 Rgba5551ToRgba8888(u16 c)
    {
        static const Rgba5551Table table;
        return table.values[c];
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    // Pages that hold indexed texture data, plus an upload-time shadow copy.
    //
    // The GS cannot rasterise into T4/T8 formats, so a raster write landing on
    // one of these pages means two guest buffers alias the same VRAM: this title
    // bases both its 512x512 T8 font atlas and a 1280x512 CT16 surface on page
    // 240 (0x1E0000), and then clears that surface with a full-surface fill
    // (16x512 strips x 80, XYOFFSET 1408/1792) whose state rules out any
    // masking: FBMSK=0, ABE=0, alpha test off, scissor 0..1279/0..511.
    //
    // The clear is real content - the surface is sampled on screen as CT16 - so
    // it must reach VRAM, otherwise the atlas bytes show up on screen (0x11
    // background reads as dark brown). Instead the indexed texture is captured
    // when it is uploaded, and once a raster write has touched its pages the
    // indexed sampling path reads that shadow copy. Both views then behave as
    // the guest intends: the colour surface is cleared, the glyphs survive.
    std::array<uint32_t, 512> g_texOwnedPages{};
    std::array<uint8_t, 512> g_texDirtyPages{};
    std::vector<uint8_t> g_texShadow;
    GSMem::TexturePageCache g_texShadowCache;
    uint32_t g_texOwnedToken = 0u;

    void forEachUploadedPage(uint32_t dbpBlocks, uint64_t bytes, const std::function<void(uint32_t)> &fn)
    {
        // One VRAM page is 8192 bytes, i.e. 32 blocks of 256 bytes. DBP arrives
        // in those 256-byte blocks.
        const uint32_t firstPage = dbpBlocks / 32u;
        const uint32_t pages = static_cast<uint32_t>((bytes + 8191u) / 8192u);
        for (uint32_t p = 0u; p < pages && firstPage + p < g_texOwnedPages.size(); ++p)
            fn(firstPage + p);
    }

    // Pages an indexed upload writes. The byte count alone undercounts them:
    // a 256x64 T4 image is 8192 bytes but its rows cross two 8 KB pages, since
    // a P4 page holds 128x128 texels and a P8 page 128x64. Walk the transfer
    // rectangle with those page extents instead.
    std::vector<uint32_t> transferPages(uint8_t dpsm, uint32_t dbw, uint32_t dsax, uint32_t dsay,
                                        uint32_t width, uint32_t height, uint32_t dbpBlocks)
    {
        std::vector<uint32_t> pages;
        if ((dpsm != GS_PSM_T4 && dpsm != GS_PSM_T8) || width == 0u || height == 0u)
            return pages;

        constexpr uint32_t kPageWidth = 128u;
        const uint32_t pageHeight = (dpsm == GS_PSM_T4) ? 128u : 64u;
        const uint32_t pagesPerRow = std::max<uint32_t>((dbw * 64u) / kPageWidth, 1u);
        const uint32_t basePage = dbpBlocks / 32u;
        for (uint32_t y = 0u; y < height; y += pageHeight)
        {
            for (uint32_t x = 0u; x < width; x += kPageWidth)
            {
                const uint32_t page = basePage + ((dsay + y) / pageHeight) * pagesPerRow +
                                      (dsax + x) / kPageWidth;
                if (page < g_texOwnedPages.size() &&
                    std::find(pages.begin(), pages.end(), page) == pages.end())
                    pages.push_back(page);
            }
        }
        return pages;
    }

    void markTextureUploadPages(const std::vector<uint32_t> &pages)
    {
        // Tokens stay valid until the guest uploads a colour image over the
        // page, so a later, unrelated texture upload cannot release the atlas.
        const uint32_t token = ++g_texOwnedToken;
        if (token == 0u)
        {
            g_texOwnedPages.fill(0u);
            g_texOwnedToken = 1u;
        }
        for (const uint32_t page : pages)
        {
            if (page >= g_texOwnedPages.size())
                continue;
            g_texOwnedPages[page] = g_texOwnedToken;
            g_texDirtyPages[page] = 0u;
        }
    }

    void captureTextureShadow(const uint8_t *vram, const std::vector<uint32_t> &pages)
    {
        if (!vram || pages.empty())
            return;
        constexpr size_t kGsVramBytes = 4u * 1024u * 1024u;
        if (g_texShadow.empty())
            g_texShadow.assign(kGsVramBytes, 0u);
        for (const uint32_t page : pages)
        {
            const size_t off = static_cast<size_t>(page) * 8192u;
            if (off + 8192u <= g_texShadow.size())
                std::memcpy(g_texShadow.data() + off, vram + off, 8192u);
        }
        g_texShadowCache.Invalidate();
    }

    void releaseTextureOwnedPages(uint32_t dbpBlocks, uint64_t bytes)
    {
        forEachUploadedPage(dbpBlocks, bytes, [](uint32_t page)
                            {
                                g_texOwnedPages[page] = 0u;
                                g_texDirtyPages[page] = 0u; });
    }

    bool isTextureOwnedPage(uint32_t page)
    {
        return page < g_texOwnedPages.size() && g_texOwnedPages[page] != 0u;
    }

    void noteRasterWriteToOwnedPage(uint32_t page)
    {
        if (isTextureOwnedPage(page))
            g_texDirtyPages[page] = 1u;
    }

    bool textureShadowNeeded(uint32_t baseBlocks)
    {
        const uint32_t page = baseBlocks / 32u;
        return !g_texShadow.empty() && isTextureOwnedPage(page) && g_texDirtyPages[page] != 0u;
    }

    // VRAM page (8 KB) a raster write lands in. `base` is the frame/zbuf base
    // in 256-byte blocks and `bw` the buffer width in 64-pixel units, exactly
    // as the PSM's page walk uses them (see PixelStorageTraits::PageId).
    uint32_t pagedAddressOfPixel(uint8_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t basePage = base / 32u;
        const uint32_t width = std::max<uint32_t>(bw, 1u);
        switch (psm & 0x3Fu)
        {
        case GS_PSM_T4:
            return basePage + (y >> 7) * std::max<uint32_t>(width >> 1, 1u) + (x >> 7);
        case GS_PSM_T8:
            return basePage + (y >> 6) * std::max<uint32_t>(width >> 1, 1u) + (x >> 7);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return basePage + (y >> 6) * width + (x >> 6);
        default:
            // CT32/CT24/Z32/Z24/P8H/P4HL/P4HH: 64x32 pages
            return basePage + (y >> 5) * width + (x >> 6);
        }
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        // Interpolation stays inside [0, 256), so truncating after a +0.5 bias
        // rounds exactly like std::lround without the libm call per channel.
        return clampU8(static_cast<int>(top + (bottom - top) * fy + 0.5f));
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

// NOTE: interlaced field output used to duplicate a single field across the
// whole frame here, which discarded every other source line. The game already
// rasterizes complete frames, so presenting them progressively keeps full
// vertical resolution (line-weaving artifacts were making text unreadable).
// The field helpers are kept as no-ops so the presentation call sites stay
// readable and a future cycle-accurate field mode can plug back in.
    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        (void)pixels;
        (void)width;
        (void)height;
        (void)oddField;
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    m_plane = ScaledPlane{};
    m_drawThroughPlane = false;
}

// ---------------------------------------------------------------------------
// Supersampled render plane (render scale > 1)
// ---------------------------------------------------------------------------

namespace
{
    uint32_t planePsmToRgba8(uint8_t psm, uint32_t raw)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
            return raw;
        case GS_PSM_CT24:
            return raw | 0x80000000u; // GS reports 0x80 dest alpha for RGB24
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return Rgba5551ToRgba8888(static_cast<u16>(raw));
        default:
            return 0u;
        }
    }

    uint32_t planeRgba8ToPsm(uint8_t psm, uint32_t rgba)
    {
        switch (psm)
        {
        case GS_PSM_CT24:
            return rgba & 0x00FFFFFFu;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return Rgba8888ToRgba5551(rgba);
        case GS_PSM_CT32:
        default:
            return rgba;
        }
    }

    uint32_t planeDepthMaskForPsm(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 0xFFFFu;
        case GS_PSM_Z24:
            return 0xFFFFFFu;
        default:
            return 0xFFFFFFFFu;
        }
    }
}

int GSCpuBackend::currentRenderScale() const
{
    return gs_video::settings().scaleOr(1);
}

bool GSCpuBackend::planeOverlapsBlocks(uint32_t blockBase, uint32_t bytes) const
{
    if (!m_plane.valid || bytes == 0u)
        return false;
    const uint32_t ownBytes = m_plane.nativeWidth * m_plane.nativeHeight * 4u;
    const uint32_t ownBlocks = ownBytes / 256u;
    const uint32_t reqBlocks = (bytes + 255u) / 256u;
    return blockBase < m_plane.fbpBlocks + ownBlocks &&
           m_plane.fbpBlocks < blockBase + reqBlocks;
}

uint32_t GSCpuBackend::planePixelAt(int x, int y) const
{
    if (!m_plane.valid || x < 0 || y < 0 ||
        static_cast<uint32_t>(x) >= m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale) ||
        static_cast<uint32_t>(y) >= m_plane.nativeHeight * static_cast<uint32_t>(m_plane.scale))
        return 0u;
    const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale);
    const size_t offset = (static_cast<size_t>(y) * scaledW + static_cast<size_t>(x)) * 4u;
    uint32_t value = 0u;
    std::memcpy(&value, m_plane.color.data() + offset, sizeof(value));
    return value;
}

void GSCpuBackend::planeSetPixel(int x, int y, uint32_t rgba)
{
    if (!m_plane.valid || x < 0 || y < 0 ||
        static_cast<uint32_t>(x) >= m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale) ||
        static_cast<uint32_t>(y) >= m_plane.nativeHeight * static_cast<uint32_t>(m_plane.scale))
        return;
    const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale);
    const size_t offset = (static_cast<size_t>(y) * scaledW + static_cast<size_t>(x)) * 4u;
    std::memcpy(m_plane.color.data() + offset, &rgba, sizeof(rgba));
}

uint32_t GSCpuBackend::planeDepthAt(int x, int y) const
{
    if (!m_plane.valid || x < 0 || y < 0 ||
        static_cast<uint32_t>(x) >= m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale) ||
        static_cast<uint32_t>(y) >= m_plane.nativeHeight * static_cast<uint32_t>(m_plane.scale))
        return 0u;
    const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale);
    return m_plane.depth[static_cast<size_t>(y) * scaledW + static_cast<size_t>(x)];
}

void GSCpuBackend::planeSetDepth(int x, int y, uint32_t z)
{
    if (!m_plane.valid || x < 0 || y < 0 ||
        static_cast<uint32_t>(x) >= m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale) ||
        static_cast<uint32_t>(y) >= m_plane.nativeHeight * static_cast<uint32_t>(m_plane.scale))
        return;
    const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale);
    m_plane.depth[static_cast<size_t>(y) * scaledW + static_cast<size_t>(x)] = z;
    m_plane.depthDirty = true;
}

void GSCpuBackend::loadPlaneUnlocked(const GSContext &ctx, int scale)
{
    if (!m_vram || m_vramSize == 0u || scale < 1)
        return;

    const uint8_t fpsm = ctx.frame.psm;
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    uint32_t nativeWidth = fbw * 64u;
    if (nativeWidth > 1024u)
        nativeWidth = 1024u; // memory cap: covers every real PS2 framebuffer
    const uint32_t nativeHeight = 512u;

    m_plane = ScaledPlane{};
    m_plane.fbpBlocks = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    m_plane.fbw = fbw;
    m_plane.psm = fpsm;
    m_plane.zbpBlocks = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    m_plane.zpsm = ctx.zbuf.psm;
    m_plane.nativeWidth = nativeWidth;
    m_plane.nativeHeight = nativeHeight;
    m_plane.scale = scale;

    const uint32_t scaledW = nativeWidth * static_cast<uint32_t>(scale);
    const uint32_t scaledH = nativeHeight * static_cast<uint32_t>(scale);
    m_plane.color.assign(static_cast<size_t>(scaledW) * scaledH * 4u, 0u);
    m_plane.depth.assign(static_cast<size_t>(scaledW) * scaledH, 0u);

    std::vector<uint8_t> rowColor(static_cast<size_t>(scaledW) * 4u);
    std::vector<uint32_t> rowDepth(scaledW, 0u);
    for (uint32_t y = 0u; y < nativeHeight; ++y)
    {
        for (uint32_t x = 0u; x < nativeWidth; ++x)
        {
            const uint32_t raw = ReadVramUnlocked(fpsm, m_plane.fbpBlocks, fbw, x, y);
            const uint32_t rgba = planePsmToRgba8(fpsm, raw);
            const uint32_t z = ReadVramUnlocked(m_plane.zpsm, m_plane.zbpBlocks, fbw, x, y);
            for (int sx = 0; sx < scale; ++sx)
            {
                const size_t colorOffset = (static_cast<size_t>(x) * scale + sx) * 4u;
                std::memcpy(rowColor.data() + colorOffset, &rgba, sizeof(rgba));
                rowDepth[static_cast<size_t>(x) * scale + sx] = z;
            }
        }
        for (int sy = 0; sy < scale; ++sy)
        {
            const size_t colorRow = (static_cast<size_t>(y) * scale + sy) * scaledW * 4u;
            std::memcpy(m_plane.color.data() + colorRow, rowColor.data(), rowColor.size());
            const size_t depthRow = (static_cast<size_t>(y) * scale + sy) * scaledW;
            std::memcpy(m_plane.depth.data() + depthRow, rowDepth.data(), rowDepth.size() * sizeof(uint32_t));
        }
    }

    m_plane.valid = true;
    m_plane.depthDirty = false;
}

void GSCpuBackend::flushPlaneUnlocked()
{
    if (!m_plane.valid)
        return;

    const int scale = m_plane.scale;
    const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(scale);
    const uint32_t area = static_cast<uint32_t>(scale) * static_cast<uint32_t>(scale);
    const uint32_t fbpBlocks = m_plane.fbpBlocks;
    const uint32_t zbpBlocks = m_plane.zbpBlocks;
    const uint32_t fbw = m_plane.fbw;
    const uint8_t psm = m_plane.psm;
    const uint8_t zpsm = m_plane.zpsm;
    const bool writeDepth = m_plane.depthDirty;

    for (uint32_t y = 0u; y < m_plane.nativeHeight; ++y)
    {
        for (uint32_t x = 0u; x < m_plane.nativeWidth; ++x)
        {
            uint32_t sumR = 0u, sumG = 0u, sumB = 0u, sumA = 0u;
            for (int sy = 0; sy < scale; ++sy)
            {
                const size_t rowBase = (static_cast<size_t>(y) * scale + sy) * scaledW +
                                       static_cast<size_t>(x) * scale;
                for (int sx = 0; sx < scale; ++sx)
                {
                    uint32_t pixel = 0u;
                    std::memcpy(&pixel, m_plane.color.data() + (rowBase + sx) * 4u, sizeof(pixel));
                    sumR += pixel & 0xFFu;
                    sumG += (pixel >> 8u) & 0xFFu;
                    sumB += (pixel >> 16u) & 0xFFu;
                    sumA += (pixel >> 24u) & 0xFFu;
                }
            }
            const uint32_t packed = pack32(static_cast<u8>(sumR / area),
                                           static_cast<u8>(sumG / area),
                                           static_cast<u8>(sumB / area),
                                           static_cast<u8>(sumA / area));
            WriteVramUnlocked(psm, fbpBlocks, fbw, x, y, planeRgba8ToPsm(psm, packed));

            if (writeDepth)
            {
                const size_t depthIndex =
                    (static_cast<size_t>(y) * scale + scale / 2) * scaledW +
                    static_cast<size_t>(x) * scale + scale / 2;
                WriteVramUnlocked(zpsm, zbpBlocks, fbw, x, y, m_plane.depth[depthIndex]);
            }
        }
    }

    m_plane = ScaledPlane{};
}

void GSCpuBackend::flushPlaneIfValid()
{
    if (m_plane.valid)
        flushPlaneUnlocked();
}

bool GSCpuBackend::preparePlaneForDraw(const GSContext &ctx, int scale)
{
    if (scale <= 1 || !m_vram)
        return false;

    const uint8_t fpsm = ctx.frame.psm;
    if (fpsm != GS_PSM_CT32 && fpsm != GS_PSM_CT24 && fpsm != GS_PSM_CT16 && fpsm != GS_PSM_CT16S)
        return false;

    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const uint32_t fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const uint8_t zpsm = ctx.zbuf.psm;

    const bool matches = m_plane.valid && m_plane.fbpBlocks == fbp && m_plane.fbw == fbw &&
                         m_plane.psm == fpsm && m_plane.zbpBlocks == zbp &&
                         m_plane.zpsm == zpsm && m_plane.scale == scale;
    if (matches)
        return true;

    if (m_plane.valid)
        flushPlaneUnlocked();

    loadPlaneUnlocked(ctx, scale);
    return m_plane.valid;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    DrawPrimitive(batch);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    // CLUT sources live in VRAM; resolve the scaled plane if it overlaps.
    if (planeOverlapsBlocks(tex0.cbp, 1024u))
        flushPlaneUnlocked();

    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
}

void GSCpuBackend::Flush()
{
    // Write back any supersampled plane so host-side readers see VRAM.
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPlaneIfValid();
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const_cast<GSCpuBackend *>(this)->flushPlaneIfValid();
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    // A raster write over an uploaded texture's pages means two guest surfaces
    // alias the same VRAM. Sample the upload-time copy instead of the clobbered
    // VRAM (see markTextureUploadPages). Palettised sampling only: a direct
    // colour sample of those pages is the guest reading its own surface, where
    // VRAM is authoritative by definition.
    const uint8_t psm8 = static_cast<uint8_t>(psm & 0x3Fu);
    if ((isEightBitIndexedPsm(psm8) || isFourBitIndexedPsm(psm8)) && textureShadowNeeded(base))
        return GSMem::ReadTexture(g_texShadowCache, g_texShadow.data(), psm, base, bw, x, y);

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPlaneIfValid();
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;

    // A raster write that lands on a page holding uploaded texture data makes
    // that texture's upload-time copy authoritative for later sampling (see
    // markTextureUploadPages). The write itself still goes through: the page is
    // simultaneously a colour surface the guest clears and displays.
    // The page number must be derived exactly like the texture walk does,
    // otherwise the dirty flag lands on a different page and the sampler keeps
    // reading the clobbered surface.
    noteRasterWriteToOwnedPage(pagedAddressOfPixel(psm, base, bw, x, y));

    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Presentation, backend switches and debug dumps all read raw VRAM, so the
    // supersampled plane must be resolved first.
    const_cast<GSCpuBackend *>(this)->flushPlaneIfValid();
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;

    // PS2_PAGEDBG=1: summarise every kind of primitive whose render target is
    // the font page (0x1E0000), so it is clear what overwrites the atlas and
    // whether glyphs are also blitted into it.
    if (const char *pd = std::getenv("PS2_PAGEDBG"); pd && pd[0] == '1' &&
        GSInternal::framePageBaseToBlock(ctx.frame.fbp) == 7680u)
    {
        struct PageSig
        {
            uint32_t type, tme, psm, fbw, texTbp0, texPsm;
        };
        static std::vector<std::pair<PageSig, uint32_t>> s_sigs;
        static uint32_t s_total = 0u;
        const PageSig sig{static_cast<uint32_t>(state.prim.type),
                          state.prim.tme ? 1u : 0u,
                          static_cast<uint32_t>(ctx.frame.psm),
                          static_cast<uint32_t>(ctx.frame.fbw),
                          ctx.tex0.tbp0,
                          static_cast<uint32_t>(ctx.tex0.psm)};
        auto it = std::find_if(s_sigs.begin(), s_sigs.end(),
                               [&](const auto &entry)
                               {
                                   const PageSig &a = entry.first;
                                   return a.type == sig.type && a.tme == sig.tme &&
                                          a.psm == sig.psm && a.fbw == sig.fbw &&
                                          a.texTbp0 == sig.texTbp0 && a.texPsm == sig.texPsm;
                               });
        if (it == s_sigs.end())
        {
            s_sigs.emplace_back(sig, 0u);
            it = std::prev(s_sigs.end());
        }
        // Full bbox in 12.4 fixed point, independent of the signature sample so
        // a clear cannot hide behind an already-seen signature.
        float minX = batch.vertices[0].x, maxX = batch.vertices[0].x;
        float minY = batch.vertices[0].y, maxY = batch.vertices[0].y;
        for (int i = 1; i < batch.vertexCount; ++i)
        {
            minX = std::min(minX, batch.vertices[i].x);
            maxX = std::max(maxX, batch.vertices[i].x);
            minY = std::min(minY, batch.vertices[i].y);
            maxY = std::max(maxY, batch.vertices[i].y);
        }
        const bool coversAtlasRows = (maxY - minY) / 16.0f >= 100.0f &&
                                     (maxX - minX) / 16.0f >= 1000.0f;
        if (coversAtlasRows)
        {
            static uint32_t s_clearN = 0u;
            static uint32_t s_clearTotal = 0u;
            ++s_clearTotal;
            if (s_clearN++ < 8u)
                std::fprintf(stderr,
                             "[pagedbg:clear] #%u type=%u tme=%u nv=%d px=(%.0f,%.0f)-(%.0f,%.0f) "
                             "rgba=%d,%d,%d,%d total=%u\n",
                             s_clearTotal, sig.type, sig.tme, batch.vertexCount,
                             minX / 16.0f, minY / 16.0f, maxX / 16.0f, maxY / 16.0f,
                             static_cast<int>(batch.vertices[0].r), static_cast<int>(batch.vertices[0].g),
                             static_cast<int>(batch.vertices[0].b), static_cast<int>(batch.vertices[0].a),
                             s_clearTotal);
        }
        ++it->second;
        ++s_total;
        if (it->second <= 4u)
        {
            std::fprintf(stderr,
                         "[pagedbg:draw] sig=%zu n=%u type=%u tme=%u nv=%d "
                         "bbox=(%.0f,%.0f)-(%.0f,%.0f) px=(%.0f,%.0f)-(%.0f,%.0f) "
                         "rgba=%d,%d,%d,%d scis=(%d,%d)-(%d,%d) psm=%u fbw=%u tex=tbp%u/psm%u "
                         "fbpsrc=%u total=%u\n",
                         static_cast<size_t>(std::distance(s_sigs.begin(), it)), it->second,
                         sig.type, sig.tme, batch.vertexCount,
                         minX, minY, maxX, maxY, minX / 16.0f, minY / 16.0f, maxX / 16.0f, maxY / 16.0f,
                         static_cast<int>(batch.vertices[0].r), static_cast<int>(batch.vertices[0].g),
                         static_cast<int>(batch.vertices[0].b), static_cast<int>(batch.vertices[0].a),
                         static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.y0),
                         static_cast<int>(ctx.scissor.x1), static_cast<int>(ctx.scissor.y1),
                         sig.psm, sig.fbw, sig.texTbp0, sig.texPsm, ctx.frame.fbp, s_total);
        }
    }

    if (const char *xd = std::getenv("PS2_XFERDBG"); xd && xd[0] == '1')
    {
        static uint32_t s_wideN = 0u;
        const int drawX1raw = static_cast<int>(batch.vertices[1].x) - static_cast<int>(ctx.xyoffset.ofx >> 4);
        if (s_wideN < 60u && (ctx.scissor.x1 > 639 || drawX1raw > 639))
        {
            ++s_wideN;
            std::fprintf(stderr,
                         "[wide] #%u type=%u v0=(%d,%d) v1=(%d,%d) ofx=%d ofy=%d "
                         "scis=(%d,%d)-(%d,%d) fbp=%u fbw=%u psm=0x%x zbp=%u\n",
                         s_wideN, static_cast<unsigned>(state.prim.type),
                         static_cast<int>(batch.vertices[0].x), static_cast<int>(batch.vertices[0].y),
                         static_cast<int>(batch.vertices[1].x), static_cast<int>(batch.vertices[1].y),
                         static_cast<int>(ctx.xyoffset.ofx >> 4), static_cast<int>(ctx.xyoffset.ofy >> 4),
                         static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.y0),
                         static_cast<int>(ctx.scissor.x1), static_cast<int>(ctx.scissor.y1),
                         ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw),
                         static_cast<unsigned>(ctx.frame.psm), ctx.zbuf.zbp);
        }
    }

    if (const char *xd = std::getenv("PS2_XFERDBG"); xd && xd[0] == '1')
    {
        static uint32_t s_covN = 0u;
        const uint32_t fbpBlocks = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        const uint32_t bpp =
            (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S ||
             ctx.frame.psm == GS_PSM_Z16 || ctx.frame.psm == GS_PSM_Z16S)
                ? 2u
                : 4u;
        const uint32_t spanBlocks =
            (std::max<uint32_t>(ctx.frame.fbw, 1u) * 64u) * 512u * bpp / 256u;
        if (fbpBlocks < 8704u && fbpBlocks + spanBlocks > 7680u && s_covN++ < 250u)
            std::fprintf(stderr,
                         "[draw:fbcov] type=%u v0=(%d,%d) v1=(%d,%d) fbp=%u->blk%u fbw=%u "
                         "psm=0x%x span=%u ofx=%d ofy=%d scis=(%d,%d)-(%d,%d) ctxt=%u "
                         "zbp=%u zpsm=0x%x\n",
                         static_cast<unsigned>(state.prim.type),
                         static_cast<int>(batch.vertices[0].x), static_cast<int>(batch.vertices[0].y),
                         static_cast<int>(batch.vertices[1].x), static_cast<int>(batch.vertices[1].y),
                         ctx.frame.fbp, fbpBlocks, static_cast<unsigned>(ctx.frame.fbw),
                         static_cast<unsigned>(ctx.frame.psm), spanBlocks,
                         static_cast<int>(ctx.xyoffset.ofx >> 4), static_cast<int>(ctx.xyoffset.ofy >> 4),
                         static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.y0),
                         static_cast<int>(ctx.scissor.x1), static_cast<int>(ctx.scissor.y1),
                         static_cast<unsigned>(state.prim.ctxt ? 1 : 0),
                         ctx.zbuf.zbp, static_cast<unsigned>(ctx.zbuf.psm));
    }

    // PS2_PAGEDBG=1: attribute damage to the font page (block 7680, 0x1E0000)
    // to the draw that performs it. Every draw whose framebuffer span reaches
    // that block is measured before and after, so the writer is named by
    // measurement instead of inferred from whichever register state looks
    // guilty.
    if (const char *pd = std::getenv("PS2_PAGEDBG"); pd && pd[0] == '1' && m_vram)
    {
        constexpr uint32_t kAtlasBase = 7680u * 256u;
        constexpr uint32_t kAtlasTexels = 512u * 512u;
        const uint32_t fbpBlocks = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        const uint32_t frameBpp =
            (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S ||
             ctx.frame.psm == GS_PSM_Z16 || ctx.frame.psm == GS_PSM_Z16S)
                ? 2u
                : 4u;
        const uint32_t spanBlocks =
            (std::max<uint32_t>(ctx.frame.fbw, 1u) * 64u) * 512u * frameBpp / 256u;
        if (fbpBlocks < kAtlasBase / 256u + 256u && fbpBlocks + spanBlocks > kAtlasBase / 256u)
        {
            struct AtlasDamageGuard
            {
                const uint8_t *page;
                uint32_t zerosBefore;
                uint32_t fbp, fbw, psm, type, tme, texTbp0, texPsm;
                int vx0, vy0, vx1, vy1;
                ~AtlasDamageGuard()
                {
                    uint32_t zerosAfter = 0u;
                    for (uint32_t i = 0; i < kAtlasTexels; ++i)
                        zerosAfter += (page[i] == 0u) ? 1u : 0u;
                    if (zerosAfter == zerosBefore)
                        return;
                    std::fprintf(stderr,
                                 "[atlas:dmg] zeros %u -> %u type=%u tme=%u fbp=%u/blk%u "
                                 "fbw=%u psm=0x%x tex=tbp%u/psm=0x%x "
                                 "v0=(%d,%d) v1=(%d,%d)\n",
                                 zerosBefore, zerosAfter, type, tme, fbp, fbp, fbw, psm,
                                 texTbp0, texPsm, vx0, vy0, vx1, vy1);
                }
            };
            uint32_t zerosBefore = 0u;
            for (uint32_t i = 0; i < kAtlasTexels; ++i)
                zerosBefore += (m_vram[kAtlasBase + i] == 0u) ? 1u : 0u;
            const AtlasDamageGuard atlasDamage{
                m_vram + kAtlasBase,
                zerosBefore,
                ctx.frame.fbp,
                static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                static_cast<uint32_t>(state.prim.type),
                state.prim.tme ? 1u : 0u,
                ctx.tex0.tbp0,
                static_cast<uint32_t>(ctx.tex0.psm),
                static_cast<int>(batch.vertices[0].x),
                static_cast<int>(batch.vertices[0].y),
                static_cast<int>(batch.vertices[1].x),
                static_cast<int>(batch.vertices[1].y)};
            (void)atlasDamage;
        }
    }

    PS2_IF_TRACE({
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });
    });

    PS2_IF_TRACE({
    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });
    });

    // Internal resolution scaling: rasterize through a supersampled plane
    // when a render scale > 1 is configured. Geometry, scissor and the draw
    // offset are scaled into plane space; texture coordinates are unaffected.
    m_drawThroughPlane = false;
    const GSPrimitiveBatch *drawBatch = &batch;
    GSPrimitiveBatch scaledBatch{};
    const int renderScale = currentRenderScale();
    if (renderScale > 1)
    {
        // Feedback: if the texture or CLUT lives in the region being scaled,
        // resolve the plane to VRAM first so texels read native-resolution data.
        if (m_plane.valid && state.prim.tme)
        {
            const GSTex0Reg &tex = ctx.tex0;
            const uint32_t texW = 1u << std::min<uint32_t>(tex.tw, 10u);
            const uint32_t texH = 1u << std::min<uint32_t>(tex.th, 10u);
            const uint32_t texBytes =
                (texW * texH * static_cast<uint32_t>(bitsPerPixel(tex.psm)) + 7u) / 8u;
            if (planeOverlapsBlocks(tex.tbp0, texBytes) ||
                planeOverlapsBlocks(tex.cbp, 1024u))
            {
                flushPlaneUnlocked();
            }
        }

        if (preparePlaneForDraw(ctx, renderScale))
        {
            scaledBatch = batch;
            auto &sctx = scaledBatch.state.context;
            for (uint8_t i = 0u; i < scaledBatch.vertexCount; ++i)
            {
                scaledBatch.vertices[i].x *= static_cast<float>(renderScale);
                scaledBatch.vertices[i].y *= static_cast<float>(renderScale);
            }
            const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(renderScale);
            const uint32_t scaledH = m_plane.nativeHeight * static_cast<uint32_t>(renderScale);
            sctx.scissor.x0 = static_cast<uint16_t>(
                std::min<uint32_t>(static_cast<uint32_t>(ctx.scissor.x0) * renderScale,
                                   scaledW != 0u ? scaledW - 1u : 0u));
            sctx.scissor.x1 = static_cast<uint16_t>(
                std::min<uint32_t>(static_cast<uint32_t>(ctx.scissor.x1) * renderScale,
                                   scaledW != 0u ? scaledW - 1u : 0u));
            sctx.scissor.y0 = static_cast<uint16_t>(
                std::min<uint32_t>(static_cast<uint32_t>(ctx.scissor.y0) * renderScale,
                                   scaledH != 0u ? scaledH - 1u : 0u));
            sctx.scissor.y1 = static_cast<uint16_t>(
                std::min<uint32_t>(static_cast<uint32_t>(ctx.scissor.y1) * renderScale,
                                   scaledH != 0u ? scaledH - 1u : 0u));
            const uint32_t ofx = (static_cast<uint32_t>(ctx.xyoffset.ofx >> 4) * renderScale) << 4;
            const uint32_t ofy = (static_cast<uint32_t>(ctx.xyoffset.ofy >> 4) * renderScale) << 4;
            sctx.xyoffset.ofx = static_cast<uint16_t>(std::min<uint32_t>(ofx, 0xFFF0u));
            sctx.xyoffset.ofy = static_cast<uint16_t>(std::min<uint32_t>(ofy, 0xFFF0u));
            drawBatch = &scaledBatch;
            m_drawThroughPlane = true;
        }
    }

    switch (drawBatch->state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(*drawBatch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(*drawBatch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(*drawBatch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = drawBatch->vertices[0];
        const auto &pctx = drawBatch->state.context;
        int px = static_cast<int>(v.x) - (pctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (pctx.xyoffset.ofy >> 4);
        WritePixel(drawBatch->state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    // When drawing through the supersampled plane, coordinates arrive in
    // plane space; anything outside the covered region is dropped.
    const bool throughPlane = m_drawThroughPlane && m_plane.valid;
    if (throughPlane)
    {
        const uint32_t scaledW = m_plane.nativeWidth * static_cast<uint32_t>(m_plane.scale);
        const uint32_t scaledH = m_plane.nativeHeight * static_cast<uint32_t>(m_plane.scale);
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= scaledW ||
            static_cast<uint32_t>(y) >= scaledH)
        {
            return;
        }
    }

    // A raster write that lands on a page holding indexed texture data makes
    // that texture's shadow copy authoritative for later sampling (see
    // markTextureUploadPages). The write itself still goes through: the page is
    // simultaneously a colour surface the guest clears and displays.
    if (!throughPlane)
        noteRasterWriteToOwnedPage(ctx.frame.fbp +
                                   (static_cast<uint32_t>(y) >> 6) * std::max<u32>(ctx.frame.fbw, 1u) +
                                   (static_cast<uint32_t>(x) >> 6));

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        if (throughPlane)
        {
            fbrgba = planePixelAt(x, y);
            rawFramebufferPixel = (bitsPerPixel(fpsm) == 16)
                                      ? Rgba8888ToRgba5551(fbrgba)
                                      : fbrgba;
        }
        else
        {
            rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
            fbrgba = rawFramebufferPixel;

            if (bitsPerPixel(fpsm) == 16)
            {
                fbrgba = Rgba5551ToRgba8888(fbrgba);
            }
            else if (fpsm == GS_PSM_CT24)
            {
                // The GS supplies 0x80 as destination alpha for RGB24 blending.
                fbrgba |= 0x80000000u;
            }
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = throughPlane ? planeDepthAt(x, y)
                                : ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = throughPlane ? planeDepthAt(x, y)
                                : ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            if (throughPlane && bitsPerPixel(fpsm) == 16)
            {
                // Apply the 16-bit write mask in packed form, then widen back
                // so the plane keeps its canonical RGBA8 layout.
                u32 packed16 = Rgba8888ToRgba5551(pixel);
                const u32 dest16 = Rgba8888ToRgba5551(fbrgba);
                const u32 mask16 = static_cast<u32>(ctx.frame.fbmsk) & 0xFFFFu;
                packed16 = (packed16 & ~mask16) | (dest16 & mask16);
                pixel = Rgba5551ToRgba8888(static_cast<u16>(packed16));
            }
            else
            {
                pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
            }
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        if (throughPlane)
        {
            planeSetPixel(x, y, pixel);
        }
        else
        {
            // format conversion
            if (bitsPerPixel(fpsm) == 16)
            {
                pixel = Rgba8888ToRgba5551(pixel);
            }

            WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
        }
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        if (throughPlane)
        {
            planeSetDepth(x, y, static_cast<uint32_t>(z) & planeDepthMaskForPsm(zpsm));
        }
        else
        {
            WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
        }
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
    {
        const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;
    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // PS2_TEXDBG=1: capture textured sprite parameters to diagnose the
        // glyph slicing (UV-vs-STQ misinterpretation shows up as a texel span
        // wildly mismatched with the on-screen sprite size).
        if (const char *texDbg = std::getenv("PS2_TEXDBG"); texDbg && texDbg[0] == '1')
        {
            static uint32_t s_texDbgSmall = 0u;
            static uint32_t s_texDbgAnomaly = 0u;
            const float spanU = std::fabs(u1f - u0f);
            const float spanV = std::fabs(v1f - v0f);
            const float stretchU = spanU / spriteW;
            const float stretchV = spanV / spriteH;
            const bool anomaly = stretchU > 3.0f || stretchU < (1.0f / 3.0f) ||
                                 spanU > static_cast<float>(texW) * 2.0f;
            // Glyph/UI-scale draws only: full-height background strips would
            // swallow the buffer before text draws are ever reached.
            const bool small = spanY <= 64 && spanX <= 640;
            if ((small && s_texDbgSmall++ < 300u) || (anomaly && s_texDbgAnomaly++ < 80u))
            {
                std::fprintf(stderr,
                             "[texdbg:spr] %dx%d px at (%d,%d) uv0=%u,%u uv1=%u,%u "
                             "stq0=%.4f,%.4f,q=%.4f stq1=%.4f,%.4f tex=%ux%u tbw=%u psm=%u cpsm=%u "
                             "fst=%d lin=%d clamp=%u,%u,%u,%u tbp0=%u cbp=%u tfx=%u tcc=%u "
                             "span=%.1fx%.1f texels stretch=%.2fx%.2f rgba=%d,%d,%d,%d\n",
                             spanX, spanY, unclippedX0, unclippedY0,
                             v0.u, v0.v, v1.u, v1.v,
                             v0.s, v0.t, v0.q, v1.s, v1.t,
                             texW, texH,
                             static_cast<unsigned>(tex.tbw), static_cast<unsigned>(tex.psm),
                             static_cast<unsigned>(tex.cpsm),
                             state.prim.fst ? 1 : 0, state.linearFilter ? 1 : 0,
                             static_cast<unsigned>(ctx.clamp & 3u),
                             static_cast<unsigned>((ctx.clamp >> 2) & 3u),
                             static_cast<unsigned>((ctx.clamp >> 4) & 0x3FFu),
                             static_cast<unsigned>((ctx.clamp >> 24) & 0x3FFu),
                             tex.tbp0, tex.cbp,
                             static_cast<unsigned>(tex.tfx), static_cast<unsigned>(tex.tcc),
                             spanU, spanV, stretchU, stretchV,
                             static_cast<int>(v1.r), static_cast<int>(v1.g),
                             static_cast<int>(v1.b), static_cast<int>(v1.a));

                // Per-glyph probe: does the font region hold uploaded data at
                // draw time? (One line per draw, first 30 glyph-sized draws.)
                if ((tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T4) && spanX <= 32 && spanY <= 32)
                {
                    static uint32_t s_glyphN = 0u;
                    if (s_glyphN++ < 30u)
                    {
                        if (const char *fdd = std::getenv("PS2_FONTDUMP_DIR");
                            fdd && fdd[0] && s_glyphN <= 4u)
                        {
                            const std::string path = std::string(fdd) + "/draw_" +
                                                     std::to_string(s_glyphN - 1u) + ".ppm";
                            DumpTexturePpm(path.c_str(), tex.psm, tex.tbp0, tex.tbw,
                                           static_cast<uint32_t>(texW),
                                           static_cast<uint32_t>(texH));
                        }
                        const int uBase = static_cast<int>(u0f);
                        const int vBase = static_cast<int>(v0f);
                        // Frame context the glyph inherits: if this is the
                        // 1280x512 CT16 UI surface (fbp=240) and the atlas
                        // also lives at page 240, the draw samples the very
                        // surface it writes into.
                        std::fprintf(stderr,
                                     "[glyph:tex] #%u this=%p vram=%p at=(%d,%d) cell=(%d,%d) "
                                     "frame=fbp%u/fbw%u/psm%u tex=tbp%u/psm%u/cbp%u kickpc=%08x kickaddr=%08x raw=",
                                     s_glyphN, static_cast<const void *>(this),
                                     static_cast<const void *>(m_vram),
                                     unclippedX0, unclippedY0, uBase, vBase,
                                     static_cast<unsigned>(ctx.frame.fbp),
                                     static_cast<unsigned>(ctx.frame.fbw),
                                     static_cast<unsigned>(ctx.frame.psm),
                                     tex.tbp0, static_cast<unsigned>(tex.psm), tex.cbp,
                                     ps2_guest_debug::last_store_pc,
                                     ps2_guest_debug::last_store_addr);
                        for (uint32_t i = 0; i < 16u; ++i)
                            std::fprintf(stderr, "%02x",
                                         ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw,
                                                                 uBase + static_cast<int>(i), vBase) & 0xFFu);
                        // Same texels through the uncached VRAM path, to tell a
                        // stale texture page cache apart from missing data.
                        std::fprintf(stderr, " linear=");
                        for (uint32_t i = 0; i < 16u; ++i)
                            std::fprintf(stderr, "%02x",
                                         ReadVramUnlocked(tex.psm, tex.tbp0, tex.tbw,
                                                          uBase + static_cast<int>(i), vBase) & 0xFFu);
                        std::fprintf(stderr, " vramBytes=");
                        for (uint32_t i = 0; i < 8u; ++i)
                            std::fprintf(stderr, "%02x", m_vram[0x1E0000u + i]);
                        std::fprintf(stderr, "\n");

                        // Whole-page census of the atlas the UVs address, so a
                        // missing glyph upload (uniform page) is distinguishable
                        // from UVs that point at the wrong cell.
                        {
                            uint32_t n11 = 0u, n00 = 0u, nOther = 0u;
                            uint8_t seen[256] = {0};
                            uint32_t distinct = 0u;
                            std::vector<uint8_t> plane(512u * 512u);
                            for (int j = 0; j < 512; ++j)
                                for (int i = 0; i < 512; ++i)
                                {
                                    const uint8_t idx = static_cast<uint8_t>(
                                        ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, i, j) & 0xFFu);
                                    plane[static_cast<size_t>(j) * 512u + static_cast<size_t>(i)] = idx;
                                    if (idx == 0x11u)
                                        ++n11;
                                    else if (idx == 0u)
                                        ++n00;
                                    else
                                        ++nOther;
                                    if (!seen[idx])
                                    {
                                        seen[idx] = 1u;
                                        ++distinct;
                                    }
                                }
                            std::fprintf(stderr,
                                         "[glyph:page] #%u 512x512 n11=%u n00=%u nother=%u distinct=%u\n",
                                         s_glyphN, n11, n00, nOther, distinct);
                            if (const char *pd = std::getenv("PS2_GLYPHPAGE_DIR");
                                pd && pd[0] && s_glyphN <= 3u)
                            {
                                const std::string path = std::string(pd) + "/page_" +
                                                         std::to_string(s_glyphN) + ".bin";
                                if (FILE *fp = std::fopen(path.c_str(), "wb"))
                                {
                                    std::fwrite(plane.data(), 1u, plane.size(), fp);
                                    std::fclose(fp);
                                    std::fprintf(stderr, "[glyphpage] wrote %s\n", path.c_str());
                                }
                            }
                        }

                        // One-shot full VRAM snapshot at glyph-draw time: shows
                        // whether any glyph atlas exists anywhere, or whether the
                        // page the draw samples is simply the only candidate.
                        if (const char *vd = std::getenv("PS2_VRAMDUMP_DIR");
                            vd && vd[0] && s_glyphN == 1u)
                        {
                            const std::string path = std::string(vd) + "/vram_draw.bin";
                            if (FILE *fp = std::fopen(path.c_str(), "wb"))
                            {
                                std::fwrite(m_vram, 1u, 4u << 20, fp);
                                std::fclose(fp);
                                std::fprintf(stderr, "[vramdump] wrote %s\n", path.c_str());
                            }
                            // EE memory alongside VRAM, so the staging buffer the
                            // font image is streamed from can be located.
                            if (ps2_guest_debug::rdram)
                            {
                                const std::string ramPath = std::string(vd) + "/rdram_draw.bin";
                                if (FILE *rfp = std::fopen(ramPath.c_str(), "wb"))
                                {
                                    std::fwrite(ps2_guest_debug::rdram, 1u, 32u << 20, rfp);
                                    std::fclose(rfp);
                                    std::fprintf(stderr, "[vramdump] wrote %s\n", ramPath.c_str());
                                }
                            }
                        }
                    }
                }

                        if (false) {
                        if (false) {
                        const uint64_t tex1 = ctx.tex1;
                        std::fprintf(stderr,
                                     "[texdbg:atlas] mmag=%u mmin=%u csm=%u csa=%u cpsm=%u cld=%u "
                                     "uBase=%u vBase=%u tex1=0x%llx tex0=0x%llx\n",
                                     static_cast<unsigned>((tex1 >> 5u) & 1u),
                                     static_cast<unsigned>((tex1 >> 6u) & 7u),
                                     static_cast<unsigned>(tex.csm),
                                     static_cast<unsigned>(tex.csa),
                                     static_cast<unsigned>(tex.cpsm),
                                     static_cast<unsigned>(tex.cld),
                                     static_cast<unsigned>(static_cast<int>(u0f)),
                                     static_cast<unsigned>(static_cast<int>(v0f)),
                                     static_cast<unsigned long long>(tex1),
                                     static_cast<unsigned long long>(*reinterpret_cast<const uint64_t *>(&tex)));
                        const int uBase = static_cast<int>(u0f);
                        const int vBase = static_cast<int>(v0f);

                        // Full-atlas reconstruction: raw T8 index plane plus
                        // the CT32 CLUT, so the sampled texture can be
                        // inspected offline as an image.
                        if (const char *atlasDir = std::getenv("PS2_ATLAS_DIR"); atlasDir && atlasDir[0])
                        {
                            std::string idxPath = std::string(atlasDir) + "/atlas_idx.bin";
                            std::string clutPath = std::string(atlasDir) + "/atlas_clut.bin";
                            if (FILE *fp = std::fopen(idxPath.c_str(), "wb"))
                            {
                                std::vector<uint8_t> plane(512u * 512u);
                                for (int j = 0; j < 512; ++j)
                                    for (int i = 0; i < 512; ++i)
                                        plane[static_cast<size_t>(j) * 512u + static_cast<size_t>(i)] =
                                            static_cast<uint8_t>(ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, i, j) & 0xFFu);
                                std::fwrite(plane.data(), 1, plane.size(), fp);
                                std::fclose(fp);
                            }
                            if (FILE *fp = std::fopen(clutPath.c_str(), "wb"))
                            {
                                for (size_t i = 0; i < 256u; ++i)
                                {
                                    const uint32_t entry = static_cast<uint32_t>(m_clut[i]) |
                                                           (static_cast<uint32_t>(m_clut[i + 256u]) << 16u);
                                    std::fwrite(&entry, 4, 1, fp);
                                }
                                std::fclose(fp);
                            }
                        }

                        for (int j = -1; j < 33; ++j)
                        {
                            std::fputs("[texdbg:idx] ", stderr);
                            for (int i = -1; i < 33; ++i)
                            {
                                const uint32_t idx = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw,
                                                                             uBase + i, vBase + j) & 0xFFu;
                                std::fprintf(stderr, "%02x", idx);
                            }
                            std::fputc('\n', stderr);
                        }
                        std::fputs("[texdbg:clut]", stderr);
                        for (int i = 0; i < 16; ++i)
                            std::fprintf(stderr, " %04x", m_clut[static_cast<size_t>(i)]);
                        std::fputc('\n', stderr);
                    }
                }
            }
        }

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    if (state.prim.tme)
    {
        if (const char *texDbg = std::getenv("PS2_TEXDBG"); texDbg && texDbg[0] == '1')
        {
            static uint32_t s_texDbgTris = 0u;
            if (s_texDbgTris++ < 60u)
            {
                const int texW = state.textureWidth;
                const int texH = state.textureHeight;
                const float spanU = std::fabs(v1.u ? static_cast<float>(v1.u >> 4) - static_cast<float>(v0.u >> 4)
                                                   : static_cast<float>(v2.u >> 4) - static_cast<float>(v0.u >> 4));
                std::fprintf(stderr,
                             "[texdbg:tri] v=(%.1f,%.1f)-(%.1f,%.1f)-(%.1f,%.1f) "
                             "uv=(%u,%u)(%u,%u)(%u,%u) stq=(%.4f,%.4f,%.4f) "
                             "tex=%ux%u tbw=%u psm=%u fst=%d lin=%d tbp=%u tfx=%u spanU~%.1f\n",
                             v0.x, v0.y, v1.x, v1.y, v2.x, v2.y,
                             v0.u, v0.v, v1.u, v1.v, v2.u, v2.v,
                             v0.s, v0.t, v0.q,
                             texW, texH,
                             static_cast<unsigned>(ctx.tex0.tbw),
                             static_cast<unsigned>(ctx.tex0.psm),
                             state.prim.fst ? 1 : 0, state.linearFilter ? 1 : 0,
                             ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tfx), spanU);
            }
        }
    }

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    for (int y = minY; y <= maxY; ++y)
    {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
            WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPlaneIfValid();

    if (gs_debug::window("PS2_TEXDBG"))
    {
        std::fprintf(stderr,
                     "[xfer] f=%u dir=%u dbp=%u dbw=%u dpsm=%u sbp=%u sbw=%u spsm=%u "
                     "dsax=%u dsay=%u rrw=%u rrh=%u ssax=%u ssay=%u\n",
                     gs_debug::frame(), static_cast<unsigned>(command.direction),
                     static_cast<unsigned>(command.bitbltbuf.dbp),
                     static_cast<unsigned>(command.bitbltbuf.dbw),
                     static_cast<unsigned>(command.bitbltbuf.dpsm),
                     static_cast<unsigned>(command.bitbltbuf.sbp),
                     static_cast<unsigned>(command.bitbltbuf.sbw),
                     static_cast<unsigned>(command.bitbltbuf.spsm),
                     static_cast<unsigned>(command.trxpos.dsax),
                     static_cast<unsigned>(command.trxpos.dsay),
                     static_cast<unsigned>(command.trxreg.rrw),
                     static_cast<unsigned>(command.trxreg.rrh),
                     static_cast<unsigned>(command.trxpos.ssax),
                     static_cast<unsigned>(command.trxpos.ssay));
    }

    if (const char *xd = std::getenv("PS2_XFERDBG"); xd && xd[0] == '1')
    {
        static uint32_t s_xferN = 0u;
        if (s_xferN++ < 400u)
        {
            static const auto s_t0 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - s_t0)
                                .count();
            std::fprintf(stderr,
                         "[xfer] #%u t=%lldms dir=%u dbp=%u dbw=%u dpsm=%u sbp=%u sbw=%u spsm=%u "
                         "dsax=%u dsay=%u rrw=%u rrh=%u ssax=%u ssay=%u\n",
                         s_xferN, static_cast<long long>(ms), static_cast<unsigned>(command.direction),
                         static_cast<unsigned>(command.bitbltbuf.dbp),
                         static_cast<unsigned>(command.bitbltbuf.dbw),
                         static_cast<unsigned>(command.bitbltbuf.dpsm),
                         static_cast<unsigned>(command.bitbltbuf.sbp),
                         static_cast<unsigned>(command.bitbltbuf.sbw),
                         static_cast<unsigned>(command.bitbltbuf.spsm),
                         static_cast<unsigned>(command.trxpos.dsax),
                         static_cast<unsigned>(command.trxpos.dsay),
                         static_cast<unsigned>(command.trxreg.rrw),
                         static_cast<unsigned>(command.trxreg.rrh),
                         static_cast<unsigned>(command.trxpos.ssax),
                         static_cast<unsigned>(command.trxpos.ssay));
        }
    }

    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPlaneIfValid();
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    // Indexed uploads claim their pages: they cannot be render targets, so any
    // raster write that lands there aliases the texture (see
    // markTextureUploadPages).
    std::vector<uint32_t> claimedPages;
    if (isEightBitIndexedPsm(dpsm) || isFourBitIndexedPsm(dpsm))
    {
        claimedPages = transferPages(dpsm, dbw, dsax, m_transfer.trxpos.dsay,
                                     rrw, m_transfer.trxreg.rrh, dbp);
        if (claimedPages.empty())
            forEachUploadedPage(dbp, sizeBytes,
                                [&claimedPages](uint32_t page)
                                { claimedPages.push_back(page); });
        markTextureUploadPages(claimedPages);
    }
    else if (dpsm == GS_PSM_CT16 || dpsm == GS_PSM_CT16S || dpsm == GS_PSM_CT24 || dpsm == GS_PSM_CT32)
    {
        // Direct-colour uploads are texture art too. This title uploads its
        // menu icons as 64x64 CT16/CT32 images into pages that its 1280-wide
        // CT16 surface (page 240) also covers, so the per-frame surface clear
        // destroys them. Claim the pages and keep the upload-time copy, so the
        // sampler can fall back to it once a raster write has landed there.
        forEachUploadedPage(dbp, sizeBytes,
                            [&claimedPages](uint32_t page)
                            { claimedPages.push_back(page); });
        markTextureUploadPages(claimedPages);
    }
    if (gs_debug::window("PS2_TEXDBG"))
    {
        std::fprintf(stderr,
                     "[upload] f=%u dbp=%u dbw=%u dpsm=%u dsax=%u dsay=%u rrw=%u rrh=%u size=%u pages=%zu\n",
                     gs_debug::frame(), dbp, dbw, static_cast<unsigned>(dpsm),
                     static_cast<unsigned>(dsax), static_cast<unsigned>(m_transfer.trxpos.dsay),
                     rrw, static_cast<unsigned>(m_transfer.trxreg.rrh), sizeBytes,
                     claimedPages.size());
    }

    uint32_t offset = 0u;

    if (const char *xd = std::getenv("PS2_XFERDBG"); xd && xd[0] == '1')
    {
        static uint32_t s_upN = 0u;
        if (s_upN++ < 400u)
        {
            static const auto s_t1 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - s_t1)
                                .count();
            std::fprintf(stderr, "[xfer:up] #%u t=%lldms bytes=%u dbp=%u dbw=%u dpsm=%u rrw=%u x=%u y=%u pc=%08x ra=%08x\n",
                         s_upN, static_cast<long long>(ms), sizeBytes, dbp, dbw,
                         static_cast<unsigned>(dpsm), rrw,
                         m_transferState.x, m_transferState.y,
                         ps2_guest_debug::last_store_pc, ps2_guest_debug::last_store_ra);
        }
    }

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }

    // Keep the upload-time copy of indexed texture pages, so a later raster
    // write to the same pages (see noteRasterWriteToOwnedPage) can be ignored
    // when the texture is sampled.
    if (!claimedPages.empty() && m_transferState.direction == 3u)
    {
        captureTextureShadow(m_vram, claimedPages);
        // The transfer's own writes reached the pages through WriteVramUnlocked
        // and marked them dirty; the content they left is the good copy, not a
        // clobber, so start the shadow from a clean state.
        markTextureUploadPages(claimedPages);
    }

    if (const char *xd = std::getenv("PS2_XFERDBG"); xd && xd[0] == '1' &&
        ((dpsm == GS_PSM_T8 || dpsm == GS_PSM_T4 || dpsm == GS_PSM_CT16) &&
         dbp >= 7000u && dbp <= 9000u))
    {
        static uint32_t s_doneN = 0u;
        if (s_doneN++ < 400u)
        {
            std::fprintf(stderr, "[xfer:done] this=%p vram=%p scale=%d plane=%d dpsm=%u copied=%u/%u input=",
                         static_cast<const void *>(this), static_cast<const void *>(m_vram),
                         currentRenderScale(), m_plane.valid ? 1 : 0,
                         static_cast<unsigned>(dpsm), m_transferState.copiedPixels,
                         m_transferState.totalPixels);
            for (uint32_t i = 0; i < 16u && i < sizeBytes; ++i)
                std::fprintf(stderr, "%02x", data[i]);
            std::fprintf(stderr, " vramAfter=");
            for (uint32_t i = 0; i < 16u; ++i)
                std::fprintf(stderr, "%02x",
                             ReadVramUnlocked(dpsm, dbp, dbw, i, 0) &
                                 (dpsm == GS_PSM_T4 ? 0xFu : 0xFFu));
            std::fprintf(stderr, " raw=");
            if (dbp == 7680u)
                for (uint32_t i = 0; i < 16u; ++i)
                    std::fprintf(stderr, "%02x", m_vram[0x1E0000u + i]);
            else
                std::fputs("n/a", stderr);
            std::fprintf(stderr, " fontHead=");
            for (uint32_t i = 0; i < 8u; ++i)
                std::fprintf(stderr, "%02x", m_vram[0x1E0000u + i]);
            std::fprintf(stderr, "\n");
        }
    }

    // PS2_FONTDUMP_DIR=<dir>: dump the font atlas right after upload so it can
    // be compared against the same region sampled at draw time.
    if (const char *fdd = std::getenv("PS2_FONTDUMP_DIR"); fdd && fdd[0] &&
        dpsm == GS_PSM_T8 && dbp == 7680u)
    {
        static uint32_t s_upN = 0u;
        if (s_upN < 4u)
        {
            const std::string path = std::string(fdd) + "/upload_" +
                                     std::to_string(s_upN++) + ".ppm";
            DumpTexturePpm(path.c_str(), GS_PSM_T8, dbp, dbw, 512u, 512u);
        }
    }
}

void GSCpuBackend::DumpTexturePpm(const char *path, uint32_t psm, uint32_t tbp0, uint32_t tbw,
                                  uint32_t width, uint32_t height)
{
    if (!path || !path[0] || !m_vram || width == 0u || height == 0u)
        return;

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u, 0u);
    const bool indexed = (psm == GS_PSM_T8 || psm == GS_PSM_T4);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            uint32_t r = 0u, g = 0u, b = 0u;
            if (indexed)
            {
                const uint32_t idx = ReadTextureVramUnlocked(psm, tbp0, tbw, x, y) &
                                     (psm == GS_PSM_T4 ? 0xFu : 0xFFu);
                const uint32_t entry = static_cast<uint32_t>(m_clut[idx]) |
                                       (static_cast<uint32_t>(m_clut[idx + 256u]) << 16u);
                r = entry & 0xFFu;
                g = (entry >> 8u) & 0xFFu;
                b = (entry >> 16u) & 0xFFu;
            }
            else
            {
                const uint32_t px = ReadVramUnlocked(psm, tbp0, tbw, x, y);
                r = px & 0xFFu;
                g = (px >> 8u) & 0xFFu;
                b = (px >> 16u) & 0xFFu;
            }
            const size_t o = (static_cast<size_t>(y) * width + x) * 3u;
            rgb[o] = static_cast<uint8_t>(r);
            rgb[o + 1u] = static_cast<uint8_t>(g);
            rgb[o + 2u] = static_cast<uint8_t>(b);
        }
    }

    if (FILE *fp = std::fopen(path, "wb"))
    {
        std::fprintf(fp, "P6\n%u %u\n255\n", width, height);
        std::fwrite(rgb.data(), 1, rgb.size(), fp);
        std::fclose(fp);
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (const char *xd = std::getenv("PS2_XFERDBG"); xd && xd[0] == '1')
    {
        static uint32_t s_clN = 0u;
        if (s_clN++ < 60u)
            std::fprintf(stderr, "[clear] fbp=%u fbw=%u psm=0x%x rgba=%08x zbp=%u\n",
                         context.frame.fbp, static_cast<unsigned>(context.frame.fbw),
                         static_cast<unsigned>(context.frame.psm), rgba, context.zbuf.zbp);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    flushPlaneIfValid();
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);

    // Raw ground-truth VRAM capture (PS2_VRAM_RAW_DIR): independent of any
    // read-path addressing, used to localize where texture uploads land.
    if (const char *rawDir = std::getenv("PS2_VRAM_RAW_DIR"); rawDir && rawDir[0])
    {
        static uint32_t s_rawN = 0u;
        ++s_rawN;
        // Watch the atlas region head: log every transition.
        if (m_vram)
        {
            static uint8_t s_lastHead[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
            if (std::memcmp(s_lastHead, m_vram + 0x1E0000u, 8u) != 0)
            {
                std::memcpy(s_lastHead, m_vram + 0x1E0000u, 8u);
                std::fprintf(stderr, "[atlas-watch] present=%u head=", s_rawN);
                for (int i = 0; i < 8; ++i)
                    std::fprintf(stderr, "%02x", s_lastHead[i]);
                std::fprintf(stderr, "\n");
            }
        }
        if (s_rawN == 90u || s_rawN == 180u || s_rawN == 600u || s_rawN == 1200u)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/vram_%06u.bin", rawDir, s_rawN);
            if (FILE *fp = std::fopen(path, "wb"))
            {
                std::fwrite(snapshot.data(), 1, snapshot.size(), fp);
                std::fclose(fp);
                std::fprintf(stderr, "[rawdump] %s (%zu bytes) this=%p vram=%p\n", path,
                             snapshot.size(), static_cast<const void *>(this),
                             static_cast<const void *>(m_vram));
            }
        }
    }

    if (snapshot.empty())
    {
        PS2_IF_TRACE({
        static uint32_t s_failSnap = 0u;
        if (++s_failSnap <= 12u || (s_failSnap % 4096u) == 0u)
            std::fprintf(stderr, "[present-fail] empty VRAM snapshot n=%u\n", s_failSnap);
        });
        return {};
    }

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
    {
        PS2_IF_TRACE({
        static uint32_t s_failNoCrt = 0u;
        if (++s_failNoCrt <= 24u || (s_failNoCrt % 4096u) == 0u)
            std::fprintf(stderr,
                         "[present-fail] no valid CRT n=%u pmode(crt1=%d crt2=%d mmod=%d alp=%u) "
                         "dispfb1=fbp%u/fbw%u/psm%u display1=%08x dispfb2=fbp%u/fbw%u/psm%u\n",
                         s_failNoCrt, pmode.enableCrt1 ? 1 : 0, pmode.enableCrt2 ? 1 : 0,
                         pmode.mmod ? 1 : 0, pmode.alp,
                         displayFrame1.fbp, displayFrame1.fbw, displayFrame1.psm, request.display1,
                         displayFrame2.fbp, displayFrame2.fbw, displayFrame2.psm);
        });
        return result;
    }

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
        if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        {
            PS2_IF_TRACE({
            static uint32_t s_failCopy = 0u;
            if (++s_failCopy <= 24u || (s_failCopy % 4096u) == 0u)
                std::fprintf(stderr,
                             "[present-fail] copySource n=%u valid1=%d valid2=%d "
                             "sel=fbp%u/fbw%u/psm%u %ux%u origin=%u,%u\n",
                             s_failCopy, valid1 ? 1 : 0, valid2 ? 1 : 0,
                             selected.fbp, selected.fbw, selected.psm,
                             result.width, result.height, origin.x, origin.y);
            });
            return {};
        }
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
