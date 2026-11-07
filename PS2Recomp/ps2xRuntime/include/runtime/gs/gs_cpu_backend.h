#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    // Supersampled render plane: when a render scale > 1 is active, primitives
    // rasterize into this scaled color/depth plane instead of GS local memory.
    // The plane is written back (box-downsampled) to VRAM whenever something
    // needs native-resolution data: texture fetches from the drawn region,
    // transfers, clears, presentation, or backend switches.
    struct ScaledPlane
    {
        bool valid = false;
        uint32_t fbpBlocks = 0u;    // FRAME base in blocks
        uint32_t fbw = 0u;          // FRAME buffer width in blocks
        uint8_t psm = 0u;           // FRAME pixel storage mode
        uint32_t zbpBlocks = 0u;    // ZBUF base in blocks
        uint8_t zpsm = 0u;          // ZBUF pixel storage mode
        uint32_t nativeWidth = 0u;  // region width in native pixels (fbw * 64)
        uint32_t nativeHeight = 0u; // region height in native pixels
        int scale = 1;
        bool depthDirty = false;
        std::vector<uint8_t> color;  // scaledW * scaledH, RGBA8888
        std::vector<uint32_t> depth; // scaledW * scaledH
    };

    void ResetUnlocked();

    // Resolution scaling (render scale > 1) plane management.
    int currentRenderScale() const;
    bool preparePlaneForDraw(const GSContext &ctx, int scale);
    void flushPlaneUnlocked();
    void flushPlaneIfValid();
    void loadPlaneUnlocked(const GSContext &ctx, int scale);
    bool planeOverlapsBlocks(uint32_t blockBase, uint32_t bytes) const;
    uint32_t planePixelAt(int x, int y) const;
    void planeSetPixel(int x, int y, uint32_t rgba);
    uint32_t planeDepthAt(int x, int y) const;
    void planeSetDepth(int x, int y, uint32_t z);
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    // Diagnostic: write a texture region (indexed through the current CLUT)
    // as a binary PPM so upload-time and draw-time contents can be compared.
    void DumpTexturePpm(const char *path, uint32_t psm, uint32_t tbp0, uint32_t tbw,
                        uint32_t width, uint32_t height);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
    ScaledPlane m_plane;
    // True only for primitives rasterized through the plane in this Submit;
    // coordinates then live in scaled plane space.
    bool m_drawThroughPlane = false;
};
