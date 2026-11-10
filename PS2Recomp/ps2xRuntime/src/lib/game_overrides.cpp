#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_calls.h"
#include "ps2_runtime_macros.h"
#include "ps2_recompiled_functions.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include "runtime/ee_scheduler.h"
#include "ps2_iop_transport.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Temporary boot diagnostic defined in ps2_runtime.cpp: dumps the ring buffer
// of the most recent guest control transfers.
extern "C" void ps2_guest_trace_dump(const char *reason);

// Translated Ghidra analysis gaps (src/lib/gap_functions/) are game-derived, so
// they are not part of the repository: they are regenerated locally and listed
// in gap_registry.inc, which declares and installs them.
#if __has_include("gap_functions/gap_registry.inc")
#include "gap_functions/gap_registry.inc"
#define PS2_GAP_REGISTRY_AVAILABLE 1
#endif

// ---- APEMOD file helpers (self-contained ISO9660 + host0 resolution) ----
namespace
{
    // A resolved APEMOD file: where the bytes live on the host and how big it is.
    // For cdrom0: files hostPath is the ISO image and base is the file's byte
    // offset inside it (LBN*2048). For host0: files hostPath is the mapped host
    // file and base is 0.
    struct ApeResolved
    {
        std::string hostPath;
        uint64_t base = 0;
        uint64_t size = 0;
        bool fromIso = false;
        bool ok = false;
    };

    bool apeReadAt(std::ifstream &f, uint64_t off, void *dst, size_t n)
    {
        f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        if (!f.good())
        {
            return false;
        }
        f.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(f.gcount()) == n;
    }

    uint32_t rdLe32(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    std::string apeLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Strip a trailing ISO version (";1") and any trailing '.' (dir padding).
    std::string apeStripVersion(std::string s)
    {
        const std::size_t semi = s.find(';');
        if (semi != std::string::npos)
        {
            s = s.substr(0, semi);
        }
        while (!s.empty() && s.back() == '.')
        {
            s.pop_back();
        }
        return s;
    }

    // Find `leaf` (already lowercase, version-stripped) in the ISO root dir.
    bool apeFindInIsoRoot(std::ifstream &f, const std::string &leafLower,
                          uint64_t &outBase, uint64_t &outSize)
    {
        uint8_t pvd[2048];
        if (!apeReadAt(f, 16ull * 2048ull, pvd, sizeof(pvd)))
        {
            return false;
        }
        if (std::memcmp(pvd + 1, "CD001", 5) != 0)
        {
            return false;
        }
        const uint8_t *rd = pvd + 156; // root directory record
        const uint32_t rootLba = rdLe32(rd + 2);
        const uint32_t rootSize = rdLe32(rd + 10);
        if (rootSize == 0 || rootSize > (8u << 20))
        {
            return false;
        }
        std::vector<uint8_t> dir(rootSize);
        if (!apeReadAt(f, static_cast<uint64_t>(rootLba) * 2048ull, dir.data(), rootSize))
        {
            return false;
        }
        size_t pos = 0;
        while (pos + 33 <= rootSize)
        {
            const uint8_t recLen = dir[pos];
            if (recLen < 34)
            {
                break;
            }
            const uint8_t idLen = dir[pos + 32];
            const uint8_t flags = dir[pos + 25];
            if (idLen > 0 && pos + 33 + idLen <= rootSize)
            {
                std::string name = apeStripVersion(
                    apeLower(std::string(reinterpret_cast<char *>(&dir[pos + 33]), idLen)));
                if (name == leafLower && (flags & 0x02u) == 0u)
                {
                    outBase = static_cast<uint64_t>(rdLe32(dir.data() + pos + 2)) * 2048ull;
                    outSize = rdLe32(dir.data() + pos + 10);
                    return true;
                }
            }
            pos += recLen;
        }
        return false;
    }

    // Resolve an APEMOD path (cdrom0:\NAME;1 or host0:rel/path) to host file +
    // byte offset + size. isoImage / hostRoot come from the runtime IoPaths.
    ApeResolved apeResolvePath(const std::string &ps2Path,
                               const std::string &isoImage,
                               const std::string &hostRoot)
    {
        ApeResolved out;
        const std::string lower = apeLower(ps2Path);
        if (lower.rfind("cdrom0", 0) == 0)
        {
            // Extract the file name after the last '\' or '/'.
            std::size_t slash = ps2Path.find_last_of("\\/");
            std::string leaf = (slash == std::string::npos) ? ps2Path : ps2Path.substr(slash + 1);
            leaf = apeStripVersion(apeLower(leaf));
            if (leaf.empty() || isoImage.empty())
            {
                return out;
            }
            std::ifstream f(isoImage, std::ios::binary);
            if (!f)
            {
                return out;
            }
            uint64_t base = 0, size = 0;
            if (!apeFindInIsoRoot(f, leaf, base, size))
            {
                return out;
            }
            out.hostPath = isoImage;
            out.base = base;
            out.size = size;
            out.fromIso = true;
            out.ok = true;
            return out;
        }
        if (lower.rfind("host0", 0) == 0)
        {
            std::string rel = ps2Path.substr(5); // skip "host0"
            while (!rel.empty() && (rel[0] == ':' || rel[0] == '/' || rel[0] == '\\'))
            {
                rel.erase(rel.begin());
            }
            // PS2 paths use backslashes; convert to host separators.
            std::replace(rel.begin(), rel.end(), '\\', '/');
            std::filesystem::path full = std::filesystem::path(hostRoot) / rel;
            std::error_code ec;
            const uint64_t sz = std::filesystem::exists(full, ec) ? std::filesystem::file_size(full, ec) : 0;
            if (ec)
            {
                return out;
            }
            out.hostPath = full.string();
            out.base = 0;
            out.size = sz;
            out.fromIso = false;
            out.ok = true; // ok even if size==0 (missing host file -> empty)
            return out;
        }
        return out;
    }

    // Read up to `len` bytes of a resolved ape file at `off` into `out`. The
    // bytes live at hostPath[base+off .. base+off+len). Returns bytes actually
    // read (0 on failure / EOF). Pure file I/O — no RDRAM access.
    size_t apeReadFileBytes(const std::string &hostPath, uint64_t base, uint64_t size,
                            uint64_t off, uint32_t len, std::vector<uint8_t> &out)
    {
        out.clear();
        if (hostPath.empty() || off >= size)
        {
            return 0;
        }
        uint64_t n = len;
        if (off + n > size)
        {
            n = size - off;
        }
        out.resize(static_cast<size_t>(n));
        std::ifstream f(hostPath, std::ios::binary);
        if (!f)
        {
            out.clear();
            return 0;
        }
        f.seekg(static_cast<std::streamoff>(base + off), std::ios::beg);
        f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(n));
        const size_t got = static_cast<size_t>(f.gcount());
        out.resize(got);
        return got;
    }
}
// ---- end APEMOD helpers ----

// Functions recovered from Ghidra gaps by re-running the recompiler with a
// minimal function list; the generated sources live in src/lib/gap_functions.

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, rdram + address, sizeof(value));
        return value;
    }

    // Temporary diagnostic helper: copy a guest NUL-terminated string into a
    // local buffer, replacing non-printable bytes so log lines stay readable.
    void readGuestCString(const uint8_t *rdram, uint32_t address, char *out,
                          size_t capacity)
    {
        if (capacity == 0u)
        {
            return;
        }
        out[0] = '\0';
        if (address == 0u || address >= PS2_RAM_SIZE)
        {
            return;
        }
        for (size_t i = 0; i + 1u < capacity; ++i)
        {
            if (address + i >= PS2_RAM_SIZE)
            {
                break;
            }
            const unsigned char c = rdram[address + i];
            if (c == 0u)
            {
                break;
            }
            out[i] = (c >= 0x20u && c < 0x7Fu) ? static_cast<char>(c) : '?';
            out[i + 1u] = '\0';
        }
    }

    void traceRecompiledCall(const char *name,
                             PS2Runtime::RecompiledFunction fn,
                             uint8_t *rdram,
                             R5900Context *ctx,
                             PS2Runtime *runtime)
    {
        static std::mutex traceMutex;
        const uint32_t raBefore = getRegU32(ctx, 31);
        const uint32_t spBefore = getRegU32(ctx, 29);
        const uint32_t pcBefore = ctx->pc;
        {
            std::lock_guard<std::mutex> lock(traceMutex);
            std::fprintf(stderr, "[Tcall] %s enter pc=%08x ra=%08x sp=%08x\n",
                         name, pcBefore, raBefore, spBefore);
        }

        fn(rdram, ctx, runtime);

        {
            std::lock_guard<std::mutex> lock(traceMutex);
            std::fprintf(stderr, "[Tcall] %s leave pc=%08x ra=%08x sp=%08x v0=%08x\n",
                         name, ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 29),
                         getRegU32(ctx, 2));
        }
    }
}
namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime,
                       const std::string &elfPath,
                       uint32_t entry,
                       uint32_t fileCrc32,
                       bool fileCrcValid)
    {

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}

#include <iostream>

namespace
{
    // Ghidra missed a small family of tiny leaf setters that are reached
    // through function-pointer tables (each sits in a gap between analysed
    // functions).  They all have the same shape:
    //   lui   v0, 0x3e
    //   lui   v1, 0x3e
    //   sw    a0, imm0(v0)
    //   jr    ra
    //   sw    a1, imm1(v1)
    // i.e. they store a callback pointer and its argument in two globals.
    void storeCallbackPair(uint8_t *rdram, R5900Context *ctx,
                           uint32_t address0, uint32_t address1)
    {
        const uint32_t value0 = getRegU32(ctx, 4);
        const uint32_t value1 = getRegU32(ctx, 5);
        std::memcpy(rdram + address0, &value0, sizeof(value0));
        std::memcpy(rdram + address1, &value1, sizeof(value1));
        ctx->pc = getRegU32(ctx, 31);
    }


    // The game's SIF RPC clients (client-data address -> bound SID), recorded
    // at BIND time by the SendCmd HLE and consulted at CALL time so requests
    // route into the ps2xIOP service modules.
    std::mutex g_clientSidMutex;
    std::unordered_map<uint32_t, uint32_t> g_clientSids;

    // Print the game's Deci2 debug message (written by FUN_002b8fb0 to the
    // Deci2 buffer) to stderr.  Length at 0x3c8b94, data at 0x3c8bcc.
    void dumpDeci2Message(uint8_t *rdram)
    {
        if (!rdram)
        {
            return;
        }
        const uint32_t length = *reinterpret_cast<const uint32_t *>(rdram + 0x3c8b94u);
        if (length < 0xCu || length > 0x110u)
        {
            return;
        }
        const uint32_t dataLen = length - 0xCu;
        static int deci2Prints = 0;
        ++deci2Prints;
        if (deci2Prints > 400)
        {
            return;
        }
        std::string message;
        message.reserve(dataLen);
        for (uint32_t i = 0; i < dataLen; ++i)
        {
            const char c = static_cast<char>(rdram[0x3c8bccu + i]);
            if (c == '\0')
            {
                break;
            }
            message.push_back(c);
        }
        if (!message.empty())
        {
            std::fprintf(stderr, "[game] %s\n", message.c_str());
        }
    }

    void applyJackieChanOverrides(PS2Runtime &runtime)
    {
        // Jackie Chan Adventures (Europe, Australia) - PAL SCES-52412
        // ELF entry: 0x00110008
        //
        // Per-game bindings go here.  The TOML-generated stubs already route
        // known SCE library functions (Sif*, sceCd*, scePad*, etc.) to runtime
        // handlers via the address-bound stub wrappers.
        //
        // Add bindings below as you discover them from Ghidra/runtime logs:
        //   runtime.registerFunction(0x00123456u, [](...) { ... });
        //   bindAddressHandler(runtime, 0x00123456u, "sceCdRead");

        // Boot-time CD init: many PAL titles call sceCdInit early and check
        // the return value.  Bind the game's wrapper so it reports success.
        // Replace 0xXXXXXX with the actual sceCdInit wrapper address from Ghidra.
        // runtime.registerFunction(0x00XXXXXXu,
        //   [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        //   {
        //     ps2_stubs::sceCdInit(rdram, ctx, runtime);
        //     setReturnS32(ctx, 2);
        //     ctx->pc = getRegU32(ctx, 31);
        //   });

        // SIF RPC bindings (common addresses across PS2 SDK builds).
        // These need to be verified against the Jackie Chan ELF in Ghidra.
        // bindAddressHandler(runtime, 0x00XXXXXXu, "SifInitRpc");
        // bindAddressHandler(runtime, 0x00XXXXXXu, "SifBindRpc");
        // bindAddressHandler(runtime, 0x00XXXXXXu, "SifCallRpc");

        // Bypass a Deci2Call polling loop at 0x2b90d0..0x2b90e8.
        // The game calls Deci2Call(code=4) via jal at 0x2b90d4, then checks
        // $s0+0xC at 0x2b90dc and loops back if non-zero.  The runtime Deci2
        // stub never updates that flag, so the loop spins forever.
        // FUN_002b8fb0 already copied the message to the Deci2 buffer
        // (length at 0x3c8b94, data at 0x3c8bcc) — print it here so the
        // game's own debug output is visible, then skip the wait loop.
        runtime.registerFunction(0x002B90D0u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            dumpDeci2Message(rdram);
            SET_GPR_S32(ctx, 2, 0);        // $v0 = 0 (Deci2Call success)
            SET_GPR_S32(ctx, 31, 0x002B90E8u); // $ra = return here
            ctx->pc = 0x002B90E8u;          // skip bnez, go to poll-loop exit
          });
        runtime.registerFunction(0x002B90D4u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            dumpDeci2Message(rdram);
            SET_GPR_S32(ctx, 2, 0);        // $v0 = 0 (Deci2Call success)
            SET_GPR_S32(ctx, 31, 0x002B90E8u); // $ra = return here
            ctx->pc = 0x002B90E8u;          // skip bnez, go to poll-loop exit
          });

        // Skip the intro-movie playback.  The main thread blocks inside the
        // EE mpeg library's decode loop FUN_002a0da0 (called from
        // FUN_002a0960 <- FUN_0025fae8), polling IPU registers
        // (0x10002000 CMD, 0x10002010 CTRL, IPU_FROM/TO DMA at
        // 0x1000B000/0x1000B400) which we do not emulate.  Observed: 80k+
        // polls inside a single 630-tick run - every frame the game tries
        // to decode a movie wedges the entire main loop and nothing ever
        // reaches the 3D render path.  Report "movie finished" immediately;\        // the setup performed by the caller (FUN_002a0960) is left intact.
        runtime.registerFunction(0x002A0DA0u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            static int mpegBypassCount = 0;
            ++mpegBypassCount;
            if (mpegBypassCount <= 5)
                std::fprintf(stderr, "[mpeg-bypass] decode loop finished instantly (n=%d)\n",
                             mpegBypassCount);
            SET_GPR_S32(ctx, 2, 1);       // $v0 = 1 ("movie complete")
            ctx->pc = getRegU32(ctx, 31); // return straight to the caller
          });

        // The movie TASK loop FUN_0025fae8 (thread FUN_0025fa98) would keep
        // looping around FUN_002a0960 until its end-state advances - state
        // that our instant-return bypass of FUN_002a0da0 never updates.  Skip
        // the whole task body and report success (its local result starts at
        // 1); the thread then runs its real tail: FUN_0025f8d8(ctx, 3) sets
        // the boot state and ExitThread() resumes the normal boot sequence.
        runtime.registerFunction(0x0025FAE8u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            static int taskBypassCount = 0;
            ++taskBypassCount;
            if (taskBypassCount <= 5)
                std::fprintf(stderr, "[mpeg-bypass] movie task finished instantly (n=%d)\n",
                             taskBypassCount);
            SET_GPR_S32(ctx, 2, 1);       // $v0 = 1 (task success)
            ctx->pc = getRegU32(ctx, 31); // return straight to the caller
          });

        // NOTE: FUN_002b9ca0 override REMOVED - it reads the SIF init flag at
        // 0x3c9140, which our sceSifSendCmd HLE now sets on SIF_CMD_INIT.

        // NOTE: The 0x2c10xx-0x2c15xx initialization chain and the
        // 0x2badxx-0x2bb0xx kernel setup overrides were REMOVED.  They disabled
        // the game's real initialization (SIF setup, callback registration),
        // which left Thread 2's callback queue at 0x3C7D8C empty forever.
        // These functions must run naturally.

        // NOTE: FUN_002bb498/entry_002bb4b8/FUN_002bb4d8/FUN_002bb538 overrides
        // REMOVED.  FUN_002bb498 is the SIF RPC dispatcher loop: it dequeues
        // commands via FUN_002bb278 and SleepThread()s when the queue is empty
        // (woken by DMA completion).  The earlier fake-callback injection would
        // have made it execute a garbage handler; the Sleep loop works correctly
        // now, so let Thread 2 sleep until real work arrives.

        // entry_002bb188 sets up boot params. Let it run naturally - it returns
        // via jr $ra to FUN_002bb080's caller (0x2a8598).
        // runtime.registerFunction(0x002bb188u, ...); // NOT needed - let it run

        // NOTE: FUN_0023a6d8 override REMOVED - it is main(argc, argv) (called
        // from the entry point at 0x110238 with argc/argv) and must run naturally.


        // NOTE: FUN_0023a770/0x23a894 overrides REMOVED - it registers C++
        // init/exception tables and terminates on its own (FUN_002bfb40).
        //
        // NOTE: FUN_002babe0 (sceSifBindRpc) override REMOVED - it blocks on a
        // sema that our sceSifSendCmd HLE signals when the BIND command is sent.
        //
        // NOTE: FUN_0029eee0 (sceCdInit) override REMOVED - it binds RPC SID
        // 0x80000592 and calls cdvdman init; both complete via the SendCmd HLE.

        // NOTE: 0x29f2xx-0x29f4xx and 0x2c1474 overrides REMOVED - these run
        // during natural initialization and must not be skipped.

        // FUN_002ba250 is the SIF CMD receive dispatcher (runs from the SIF0
        // DMA-done IRQ, dispatching packets via the table at 0x3c8fa4).  The
        // runtime's sceSifSetDma stub currently fires this handler with the
        // game's own outgoing buffer, which spun forever inside the 0x2bad20
        // packet-queue code.  The IOP never sends us real packets, so
        // neutralize the dispatcher: return immediately.
        runtime.registerFunction(0x002BA250u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            (void)rdram;
            (void)runtime;
            SET_GPR_S32(ctx, 2, 0);
            ctx->pc = getRegU32(ctx, 31);   // jr $ra
          });

        // FUN_002bf240 is the loadfile-module version check: it compares the
        // RPC 0xFF status response against version strings set up during init
        // and fails the whole SifLoadFile call when they mismatch.  Our HLE
        // loadfile module reports no version, so make the check always pass.
        runtime.registerFunction(0x002BF240u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            (void)rdram;
            (void)runtime;
            SET_GPR_S32(ctx, 2, 0);          // versions compatible
            ctx->pc = getRegU32(ctx, 31);    // jr $ra
          });

        // APEMOD file protocol emulation: the game's work submissions
        // (cmd 1 to the IOP) include file operations. The work type is at
        // pkt+0x0C: 3=open, 4=open2, 5=close, 6=read, 0x16=seek.
        // The IOP driver (APEMOD) handles these and responds with the result.
        struct ApeFileEntry
        {
            std::string hostPath;      // host file holding the bytes (ISO image or host0 file)
            uint64_t base = 0;         // byte offset of file start within hostPath
            uint64_t size = 0;         // real file size in bytes
            uint64_t offset = 0;       // current read offset (after open/seek)
            bool fromIso = false;
            bool open = false;
        };
        static std::mutex s_apeFileMutex;
        static std::unordered_map<uint32_t, ApeFileEntry> s_apeFiles;
        static uint32_t s_apeNextHandle = 1;
        static uint32_t s_apeCurrentHandle = 0; // last opened file (seek/sync target)
        static uint32_t s_apeLastChunkLen = 0;  // bytes loaded by the last seek
        // Type-8 completion callbacks (FUN_0021e2d0 submits them with
        // entry_0021ed18 as "job done" and entry_0021ed30 as "progress").
        // The CFIO state machine (FUN_0021f380) polls DAT_003317d4 in state 3
        // and never completes unless the driver invokes the flag callback
        // (entry_0021ed18 writes DAT_003317d4=1; entry_0021ed30 copies result
        // word3 at 0x3ecf2c into DAT_003317c0).
        static uint32_t s_apeCbFlag = 0;  // +14 callback: load-done flag
        static uint32_t s_apeCbProg = 0;  // +18 callback: progress copy
        // Fixed EE scratch buffer the HLE streams file content into. The seek
        // response returns [scratch, len]; the game points its VU1 work at it.
        // 0x01000000 (16MB) is well above the game's own data (~0x2C0000-0x410000).
        constexpr uint32_t kApeScratchAddr = 0x01000000u;
        constexpr uint32_t kApeScratchSize = 0x00200000u; // 2MB
        // The audio driver hands the game IOP addresses for its streaming
        // ring buffers, and the game then SIF-DMAs mix chunks there.  Reserve
        // the top 256KB of IOP RAM (above the 0x80000-0x200000 iopheap window
        // only at its very end) so those transfers stay inside IOP memory:
        // four streams of 64KB each.  Nothing in the HLE reads them back.
        constexpr uint32_t kApeStreamBase = 0x001C0000u;
        constexpr uint32_t kApeStreamSize = 0x00040000u;
        constexpr uint32_t kApeStreamStride = 0x00010000u;
        constexpr uint32_t kApeStreamCount = 4u;

        // CFIO pump diagnostics: FUN_0021fd48 (load orchestrator) spins on
        // FUN_0021f380 until its 4-state machine completes; state 3 waits for
        // the type-8 done flag (DAT_003317d4). Log state transitions and
        // pump entry/exit to see where the loader stalls after the burst.
        {
            static auto sOrigF380 = runtime.lookupFunction(0x0021F380u);
            static auto sOrigFd48 = runtime.lookupFunction(0x0021FD48u);
            runtime.registerFunction(0x0021F380u,
              [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
              {
                  static uint32_t sLast = 0xFFFFFFFFu;
                  static uint32_t sCount = 0u;
                  ++sCount;
                  const uint32_t st = READ32(0x3317E0u);
                  const uint32_t d4 = READ8(0x3317D4u);
                  const uint32_t b8 = READ32(0x3317B8u);
                  const uint32_t d8 = READ32(0x3317D8u);
                  const uint32_t cur = st | (d4 << 8) | (b8 << 16) | (d8 << 24);
                  if (cur != sLast)
                  {
                      PS2_IF_TRACE({ std::fprintf(stderr, "[cfio] poll n=%u state=%u d4=%u b8=%u d8=%u\n",
                                   sCount, st, d4, b8, d8); });
                      sLast = cur;
                  }
                  if (sOrigF380)
                  {
                      sOrigF380(rdram, ctx, runtime);
                  }
              });
            runtime.registerFunction(0x0021FD48u,
              [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
              {
                  PS2_IF_TRACE({ std::fprintf(stderr, "[cfio] pump enter a1=%08x a2=%u\n",
                               getRegU32(ctx, 5), getRegU32(ctx, 6)); });
                  if (sOrigFd48)
                  {
                      sOrigFd48(rdram, ctx, runtime);
                  }
                  PS2_IF_TRACE({ std::fprintf(stderr, "[cfio] pump exit ret=%08x\n", getRegU32(ctx, 2)); });
              });
        }
        // Recovered gap handlers (derive_gap_map.py -> ps2_recomp ->
        // gen_gap_registry.py), including the object-list init prologue at
        // 0x267d28 and the vtable target at 0x266da8 that Ghidra left as
        // unmapped gaps.  A tree without the generated translation still
        // builds, it just installs fewer handlers.
#ifdef PS2_GAP_REGISTRY_AVAILABLE
        ps2_gap_registry::registerHandlers(runtime);
#endif
        // HLE sceSifSendCmd (0x2ba1d0).  Args: $a0=cmd, $a1=pkt, $a2=size,
        // $a3=srcExtra, $t0=dstExtra, $t1=sizeExtra.  The game's SIF RPC stack
        // sends INIT/BIND/CALL commands to the IOP and then blocks on a sema
        // or a memory flag until the IOP answers.  Nothing drives SIF CMD
        // responses yet, so answer synchronously here:
        //   - SIF_CMD_INIT  (0x80000002): set the init flag at 0x3c9140 that
        //     SifInitRpc polls via FUN_002b9ca0.
        //   - SIF_CMD_BIND  (0x80000009): set cd->serv (+0x24) to a fake
        //     non-zero server id and signal the bind sema (cd+0x08).
        //   - SIF_CMD_RPC   (0x8000000a): complete the call and signal the
        //     call sema (cd+0x08).  Recv data stays zeroed until real CD
        //     handlers are implemented.
        runtime.registerFunction(0x002BA1D0u,
          [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
          {
            (void)rdram;
            const uint32_t cmd = getRegU32(ctx, 4);
            const uint32_t pkt = getRegU32(ctx, 5);
            if (runtime && (cmd == 0x80000002u || cmd == 0x80000009u ||
                            cmd == 0x8000000Au || cmd <= 7u))
            {
              if (cmd == 0x80000002u)
              {
                // SIF_CMD_INIT: flag polled by FUN_002b9ca0 (0x3c9140).
                WRITE32(0x3C9140u, 1);
              }
              else if (cmd <= 7u)
              {
                // The game's work-submission protocol (decoded):
                //   FUN_0023cbb8/cc98: flag[4]=0, workType=3/4 -> DAT_0033778c,
                //     path copied to pkt+0x30, SendCmd(cmd=1, pkt, 0x70),
                //     spin flag[4]==1.
                //   FUN_0023cd78: flag[4]=0, desc+0x0c=5 (work type 5),
                //     SendCmd(cmd=1...), spin flag[4]==1.
                // The IOP driver (APEMOD) processes the work and responds with
                // command 4, whose EE-side handler FUN_0023c758 copies the
                // 0x20-byte result from pkt+0x10 and sets flag[4]=1 (unless
                // the response's +0x10 == 8, meaning "not done yet").
                // Emulate that response: set flag[4] and deliver the result.
                // Shared by the [workcmd] dump and the [worktype] annotation.
                static int cmdDumpCount = 0;
                ++cmdDumpCount;
                PS2_IF_TRACE({
                // PS2_WORK_DUMP=1 dumps every work packet; otherwise first 40.
                static const bool s_dumpAll =
                    (std::getenv("PS2_WORK_DUMP") != nullptr);
                if (s_dumpAll || cmdDumpCount <= 40)
                {
                  std::fprintf(stderr, "[workcmd] cmd=%u pkt=%08x dump:\n", cmd, pkt);
                  for (uint32_t i = 0; i < 0x70u; i += 16)
                  {
                    std::fprintf(stderr, "[workcmd]   +%02x: %08x %08x %08x %08x\n", i,
                                 READ32(pkt + i), READ32(pkt + i + 4), READ32(pkt + i + 8), READ32(pkt + i + 12));
                  }
                }
                });
                // The APEMOD file protocol: emulate the file operations.
                const uint32_t workType = READ32(pkt + 0x0Cu);
                // Annotate every work submission with its decoded type + params
                // so the read/sync data path is visible in the log.
                PS2_IF_TRACE({
                  static const char *kTypeName[] = {
                      "0:INIT", "1:?", "2:?", "3:OPEN", "4:OPEN2", "5:CLOSE",
                      "6:GETSIZE", "7:?", "8:READ", "9", "0xA", "0xB", "0xC",
                      "0xD", "0xE", "0xF", "0x10:STAT", "0x11", "0x12", "0x13",
                      "0x14", "0x15", "0x16:SEEK", "0x17", "0x18:SYNC", "0x19:AREAD"};
                  const char *tn = (workType < 26u) ? kTypeName[workType] : "?";
                  std::fprintf(stderr,
                               "[worktype] #%d cmd=%u type=0x%x(%s) +10=%08x +14=%08x +18=%08x +1c=%08x\n",
                               cmdDumpCount, cmd, workType, tn,
                               READ32(pkt + 0x10u), READ32(pkt + 0x14u),
                               READ32(pkt + 0x18u), READ32(pkt + 0x1Cu));
                });
                // Handler-scope helpers that stream file bytes into EE memory
                // (WRITE32 needs rdram/runtime/ctx, so these must live here).
                auto apeWriteToEe = [&](const ApeFileEntry &fe, uint64_t off, uint32_t len,
                                        uint32_t destAddr) -> uint32_t
                {
                    std::vector<uint8_t> buf;
                    const size_t got = apeReadFileBytes(fe.hostPath, fe.base, fe.size, off, len, buf);
                    // PS2_APEDBG=1: name the file/offset feeding EE memory so a
                    // suspicious fill pattern can be traced to its source.
                    if (const char *ad = std::getenv("PS2_APEDBG"); ad && ad[0] == '1')
                    {
                        std::fprintf(stderr,
                                     "[ape:read] %s base=0x%llx size=0x%llx off=0x%llx len=0x%x dest=0x%08x got=0x%zx head=",
                                     fe.hostPath.c_str(),
                                     static_cast<unsigned long long>(fe.base),
                                     static_cast<unsigned long long>(fe.size),
                                     static_cast<unsigned long long>(off), len, destAddr, got);
                        for (size_t k = 0; k < 16u && k < buf.size(); ++k)
                            std::fprintf(stderr, "%02x", buf[k]);
                        std::fprintf(stderr, "\n");
                    }
                    size_t i = 0;
                    for (; i + 4 <= got; i += 4)
                    {
                        const uint32_t w = static_cast<uint32_t>(buf[i]) |
                                           (static_cast<uint32_t>(buf[i + 1]) << 8) |
                                           (static_cast<uint32_t>(buf[i + 2]) << 16) |
                                           (static_cast<uint32_t>(buf[i + 3]) << 24);
                        WRITE32(destAddr + static_cast<uint32_t>(i), w);
                    }
                    for (; i < got; ++i)
                    {
                        WRITE8(destAddr + static_cast<uint32_t>(i), buf[i]);
                    }
                    return static_cast<uint32_t>(got);
                };
                auto apeLoadChunk = [&](ApeFileEntry &fe, uint32_t scratchAddr) -> uint32_t
                {
                    const uint64_t remaining = (fe.size > fe.offset) ? (fe.size - fe.offset) : 0ull;
                    uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(remaining, kApeScratchSize));
                    if (len == 0u)
                    {
                        return 0u;
                    }
                    const uint32_t got = apeWriteToEe(fe, fe.offset, len, scratchAddr);
                    fe.offset += got;
                    return got;
                };
                {
                  std::lock_guard<std::mutex> lock(s_apeFileMutex);
                  switch (workType)
                  {
                  case 3: // open
                  case 4: // open (alternative mode)
                  {
                    // The path at pkt+0x30 (NUL-terminated, max 0x3F chars)
                    char path[64];
                    for (int ci = 0; ci < 63; ++ci)
                    {
                        const char c = static_cast<char>(READ8(pkt + 0x30u + ci));
                        path[ci] = c;
                        if (c == '\0') break;
                    }
                    path[63] = '\0';
                    const std::string cdPathStr = runtime->getIoPaths().cdImage.string();
                    const std::string hostRootStr = runtime->getIoPaths().hostRoot.string();
                    const ApeResolved resolved = apeResolvePath(path, cdPathStr, hostRootStr);
                    static int openLogs = 0;
                    ++openLogs;
                    if (openLogs <= 12)
                    {
                        std::fprintf(stderr, "[ape-open] type=%u path='%s' -> ok=%d base=%llx size=%llx\n",
                                     workType, path, resolved.ok ? 1 : 0,
                                     static_cast<unsigned long long>(resolved.base),
                                     static_cast<unsigned long long>(resolved.size));
                    }
                    const uint32_t handle = s_apeNextHandle++;
                    ApeFileEntry e;
                    e.open = true;
                    e.hostPath = resolved.hostPath;
                    e.base = resolved.base;
                    e.size = resolved.size;
                    e.offset = 0;
                    e.fromIso = resolved.fromIso;
                    s_apeFiles[handle] = std::move(e);
                    s_apeCurrentHandle = handle; // seek/sync/type6 default target
                    // Open result (0x20 bytes -> 0x3ecf20, parsed by FUN_0021dcb8):
                    //   word0 = status (0 = success; NON-ZERO makes the game take
                    //           its error path and skip ALL subsequent file reads)
                    //   word1 = file id (fileState+0x10) -> passed back in type-6 reads
                    //   word2 = file size (fileState+0x18) -> used to clamp reads
                    WRITE32(pkt + 0x10u, 0u);
                    WRITE32(pkt + 0x14u, handle);
                    WRITE32(pkt + 0x18u, static_cast<uint32_t>(resolved.size));
                    break;
                  }
                  case 5: // close
                  {
                    const uint32_t handle = READ32(pkt + 0x10u);
                    s_apeFiles.erase(handle);
                    WRITE32(pkt + 0x10u, 0u); // ok
                    break;
                  }
                  case 6: // get file size / read (see FUN_0023cdf8 caller FUN_0021df10)
                  {
                    // +0x10 = file id (0 -> use current stream), +0x14 = offset,
                    // +0x18 = dest EE addr, +0x1c = length. When length==0 this
                    // acts as a size query (write real size to +0x18).
                    const uint32_t dest = READ32(pkt + 0x18u);
                    const uint32_t len = READ32(pkt + 0x1Cu);
                    const uint32_t off = READ32(pkt + 0x14u);
                    const uint32_t fileId = READ32(pkt + 0x10u);
                    auto it = s_apeFiles.find(fileId);
                    if (it == s_apeFiles.end())
                    {
                        it = s_apeFiles.find(s_apeCurrentHandle);
                    }
                    const uint64_t fsize = (it != s_apeFiles.end()) ? it->second.size : 0u;
                    if (dest != 0u && dest < 0x02000000u)
                    {
                        if (len == 0u)
                        {
                            WRITE32(dest, static_cast<uint32_t>(fsize)); // size query
                        }
                        else if (it != s_apeFiles.end())
                        {
                            apeWriteToEe(it->second, off, len, dest); // real read
                        }
                    }
                    WRITE32(pkt + 0x10u, 0u); // result[0] = success
                    PS2_IF_TRACE({
                    static int sizeLogs = 0;
                    ++sizeLogs;
                    if (sizeLogs <= 12)
                    {
                        std::fprintf(stderr, "[ape-io] type6 file=%u dest=%08x off=%x len=%u size=%llx\n",
                                     fileId, dest, off, len,
                                     static_cast<unsigned long long>(fsize));
                    }
                    });
                    break;
                  }
                  case 1: // pad button/status query (FUN_0023c920, queue 2)
                  {
                    // The wrapper reads the 8-byte status block at 0x337858:
                    //   [0]=status (0 -> wrapper memsets its 0x101 table =
                    //       no buttons; nonzero = report present), [1]=mode
                    //       (1 = build table), [3]=entry-count,
                    //   [6..]=pressed button indices (one per count entry).
                    // Report the pad as present with no pressed buttons: the
                    // recompiled pad path (scePadGetState/Read) is the real
                    // input source, so this debug listing stays empty instead
                    // of injecting synthetic key presses of its own.
                    WRITE32(pkt + 0x10u, 0x01000100u); // status=1,mode=1,count=0
                    WRITE32(pkt + 0x14u, 0u);
                    PS2_IF_TRACE({
                    static int t1Logs = 0;
                    ++t1Logs;
                    if (t1Logs <= 4 || (t1Logs % 256u) == 0u)
                    {
                      std::fprintf(stderr, "[ape-t1] #%d pad-status query\n", t1Logs);
                    }
                    });
                    break;
                  }
                  case 2: // pad analog calibration query (FUN_0023ca80, queue 3)
                  {
                    // The wrapper copies 16 bytes from 0x337878 into the
                    // caller's struct (stick center/min/max words).  All-zero
                    // = zero calibration, which the caller clamps safely.
                    WRITE32(pkt + 0x10u, 0u);
                    PS2_IF_TRACE({
                    static int t2Logs = 0;
                    ++t2Logs;
                    if (t2Logs <= 4 || (t2Logs % 256u) == 0u)
                    {
                      std::fprintf(stderr, "[ape-t2] #%d analog-cal query\n", t2Logs);
                    }
                    });
                    break;
                  }
                  case 0x17: // connected-device bitmask (FUN_0023d628, queue 6, async)
                  {
                    // The caller registers its response struct at DAT_002e29c0
                    // and the real queue-6 handler (FUN_0023c818) copies result
                    // words 3/4 (0x337894/0x337898) into it: a 64-bit slot
                    // bitmask.  Deliver a deterministic "no hot-plug devices"
                    // mask instead of stale packet-pool bytes (the old default
                    // case left +0x1c/+0x20 as garbage like 0x21da68, which
                    // made the game see phantom device connects/disconnects).
                    WRITE32(pkt + 0x10u, 0u);
                    WRITE32(pkt + 0x14u, 0u);
                    WRITE32(pkt + 0x18u, 0u);
                    WRITE32(pkt + 0x1Cu, 0u); // result[3] = mask lo
                    WRITE32(pkt + 0x20u, 0u); // result[4] = mask hi
                    PS2_IF_TRACE({
                    static int t17Logs = 0;
                    ++t17Logs;
                    if (t17Logs <= 4 || (t17Logs % 256u) == 0u)
                    {
                      std::fprintf(stderr, "[ape-t17] #%d device-mask query\n", t17Logs);
                    }
                    });
                    break;
                  }
                  case 0x16: // buffer query (no params) or seek -> load a chunk
                  {
                    // FUN_0023d5e0 asks with every parameter zero and reads the
                    // reply as [IOP buffer address, buffer size] for the sound
                    // driver; the loader's seek passes real parameters.
                    if (READ32(pkt + 0x10u) == 0u && READ32(pkt + 0x14u) == 0u &&
                        READ32(pkt + 0x18u) == 0u && READ32(pkt + 0x1Cu) == 0u)
                    {
                        WRITE32(pkt + 0x10u, kApeStreamBase);
                        WRITE32(pkt + 0x14u, kApeStreamSize);
                        WRITE32(pkt + 0x18u, kApeStreamSize);
                        PS2_IF_TRACE({
                        static int bufLogs = 0;
                        ++bufLogs;
                        if (bufLogs <= 8)
                        {
                            std::fprintf(stderr, "[ape-buf] stream window base=%08x size=%u\n",
                                         kApeStreamBase, kApeStreamSize);
                        }
                        });
                        break;
                    }
                    auto it = s_apeFiles.find(s_apeCurrentHandle);
                    uint32_t ptr = kApeScratchAddr;
                    uint32_t len = 0;
                    if (it != s_apeFiles.end())
                    {
                        ApeFileEntry &fe = it->second;
                        fe.offset = 0; // seek to start
                        len = apeLoadChunk(fe, ptr);
                        s_apeLastChunkLen = len;
                    }
                    // Response payload -> routed to 0x337888 by the flag[6] path.
                    WRITE32(pkt + 0x10u, ptr); // word0: buffer pointer
                    WRITE32(pkt + 0x14u, len); // word1: buffer length
                    WRITE32(pkt + 0x18u, len); // word2: (also read by sync consumer)
                    PS2_IF_TRACE({
                    static int seekLogs = 0;
                    ++seekLogs;
                    if (seekLogs <= 8)
                    {
                        std::fprintf(stderr, "[ape-seek] -> buf=%08x len=%u\n", ptr, len);
                    }
                    });
                    break;
                  }
                  case 0x18: // per-stream buffer address (FUN_0023d670)
                  {
                    // The game reads reply word2 as an IOP address and later
                    // SIF-DMAs mix chunks there, so it must be a real IOP
                    // address: report one reserved ring buffer per stream
                    // instead of a byte count (which made the game DMA to
                    // 0x200000, outside IOP memory, and spin forever on
                    // sceSifSetDma returning 0).
                    const uint32_t stream = READ32(pkt + 0x10u);
                    const uint32_t index = stream % kApeStreamCount;
                    const uint32_t bufAddr = kApeStreamBase + index * kApeStreamStride;
                    WRITE32(pkt + 0x10u, kApeStreamBase);
                    WRITE32(pkt + 0x14u, kApeStreamSize);
                    WRITE32(pkt + 0x18u, bufAddr); // word2 = stream buffer
                    PS2_IF_TRACE({
                    static int syncLogs = 0;
                    ++syncLogs;
                    if (syncLogs <= 12)
                    {
                        std::fprintf(stderr, "[ape-stream] index=%u -> buf=%08x\n", stream, bufAddr);
                    }
                    });
                    break;
                  }
                  case 8: // register completion callbacks (FUN_0023d038)
                  {
                    // +10 = mode, +14 = "job done" callback (sets
                    // DAT_003317d4=1), +18 = "progress" callback (copies
                    // result word3 to DAT_003317c0). Stored for dispatch at
                    // the type-0xA job sync point; invoked there because
                    // FUN_0021ed48 clears DAT_003317d4 AFTER the type-8
                    // submission returns, so firing during the submission
                    // itself would be wiped.
                    const uint32_t mode8 = READ32(pkt + 0x10u);
                    s_apeCbFlag = READ32(pkt + 0x14u);
                    s_apeCbProg = READ32(pkt + 0x18u);
                    WRITE32(pkt + 0x10u, 0u);
                    PS2_IF_TRACE({
                    static int t8Logs = 0;
                    ++t8Logs;
                    if (t8Logs <= 8)
                    {
                        std::fprintf(stderr,
                                     "[ape-t8] mode=%x cbFlag=%08x cbProg=%08x\n",
                                     mode8, s_apeCbFlag, s_apeCbProg);
                    }
                    });
                    break;
                  }
                  case 9: // streaming chunk read (FUN_0023d0d0):
                  {
                    // +10 = offset in the current file, +14 = EE dest,
                    // +18 = length; no fileId — the wrapper always waits on
                    // flag[4] and copies the 0x20-byte result. The loader
                    // fires ~1000 of these to pull DATA1 content into EE
                    // memory (offsets continue sequentially from the type-6
                    // reads). The old default: case returned success without
                    // moving any bytes, so no content ever reached the game.
                    const uint32_t off9 = READ32(pkt + 0x10u);
                    const uint32_t dest9 = READ32(pkt + 0x14u);
                    const uint32_t len9 = READ32(pkt + 0x18u);
                    auto it9 = s_apeFiles.find(s_apeCurrentHandle);
                    uint32_t got9 = 0u;
                    if (it9 != s_apeFiles.end() && dest9 != 0u &&
                        dest9 < 0x02000000u && len9 != 0u)
                    {
                        got9 = apeWriteToEe(it9->second, off9, len9, dest9);
                    }
                    WRITE32(pkt + 0x10u, 0u);   // result[0] = status ok
                    WRITE32(pkt + 0x1Cu, got9); // result[3] = bytes read
                    PS2_IF_TRACE({
                    static int t9Logs = 0;
                    ++t9Logs;
                    if (t9Logs <= 12 || (t9Logs % 256u) == 0u)
                    {
                        std::fprintf(stderr,
                                     "[ape-t9] #%d off=%x dest=%08x len=%u got=%u\n",
                                     t9Logs, off9, dest9, len9, got9);
                    }
                    });
                    break;
                  }
                  case 10: // job sync point (FUN_0023d168, no params)
                  {
                    // The loader fires this after a full type-9 burst. Fire
                    // the registered completion callbacks: progress first
                    // (DAT_003317c0 = result word3), then the done flag
                    // (DAT_003317d4 = 1) which FUN_0021f380 state 3 waits on.
                    WRITE32(pkt + 0x10u, 0u);
                    auto apeInvoke = [&](uint32_t addr)
                    {
                        if (addr == 0u)
                        {
                            return;
                        }
                        if (runtime->hasFunction(addr))
                        {
                            R5900Context cbCtx{};
                            cbCtx.pc = addr;
                            runtime->lookupFunction(addr)(rdram, &cbCtx, runtime);
                        }
                        else
                        {
                            PS2_IF_TRACE({ std::fprintf(stderr, "[ape-cb] missing callback %08x\n", addr); });
                        }
                    };
                    apeInvoke(s_apeCbProg);
                    apeInvoke(s_apeCbFlag);
                    PS2_IF_TRACE({
                    static int tALogs = 0;
                    ++tALogs;
                    if (tALogs <= 8)
                    {
                        std::fprintf(stderr, "[ape-tA] job sync -> fired cbProg/cbFlag\n");
                    }
                    });
                    break;
                  }
                  default:
                  {
                    // Unknown work type: just signal done
                    WRITE32(pkt + 0x10u, 0u);
                    break;
                  }
                  }
                }
                // Flag-aware result routing. Each work type signals a specific
                // completion flag; flag[4] ops deliver their 0x20-byte result to
                // 0x3ecf20 (handler FUN_0023c758), flag[6] ops to 0x337888
                // (handler FUN_0023c818). The old code sledgehammered all 8 flags
                // and always wrote 0x3ecf20, so seek/sync results never reached
                // the game (they read 0x337888) -> content never loaded.
                {
                    uint32_t flagIndex = 4;
                    uint32_t resultBuf = 0x3ECF20u;                    if (workType == 0u)
                    {
                      flagIndex = 0; resultBuf = 0;
                    }
                    else if (workType == 1u)
                    {
                      // queue 2: wrapper reads the status block at 0x337858
                      flagIndex = 2; resultBuf = 0x337858u;
                    }
                    else if (workType == 2u)
                    {
                      // queue 3: wrapper copies 16 bytes from 0x337878
                      flagIndex = 3; resultBuf = 0x337878u;
                    }
                    else if (workType == 0xDu || workType == 0x10u || workType == 0x14u)
                    {
                      // FUN_0023d268 submits the DMA-style type 0xD work item
                      // after clearing flag[5] and spins on it (FUN_0023c4e8).
                      flagIndex = 5; resultBuf = 0;
                    }
                    else if (workType >= 0x16u && workType <= 0x19u)
                    {
                      flagIndex = 6; resultBuf = 0x337888u;
                    }
                    if (resultBuf != 0u)
                    {
                      const uint32_t copyLen =
                          (workType == 1u) ? 8u :
                          (workType == 2u) ? 0x10u : 0x20u;
                      for (uint32_t i = 0; i < copyLen; i += 4)
                      {
                        WRITE32(resultBuf + i, READ32(pkt + 0x10u + i));
                      }
                    }
                    WRITE8(0x337850u + flagIndex, 1u);
                    // A submitter clears its queue's flag (FUN_0023c548) right
                    // before handing the work item over, so any flag that is
                    // still clear belongs to work this reply completes too
                    // (3:OPEN, 0xD/0xE DMA items and friends share the channel
                    // with the type-routed queues above).
                    for (uint32_t i = 0u; i < 8u; ++i)
                    {
                      if (i != flagIndex && READ8(0x337850u + i) == 0u)
                        WRITE8(0x337850u + i, 1u);
                    }
                    // queue-6 extra routing (FUN_0023c818): the device-mask
                    // response words are also written through DAT_002e29c0
                    // (registered by FUN_0023d628's caller).
                    if (workType == 0x17u)
                    {
                      const uint32_t maskDst = READ32(0x002E29C0u);
                      if (maskDst != 0u)
                      {
                        WRITE32(maskDst, READ32(0x337894u));
                        WRITE32(maskDst + 4u, READ32(0x337898u));
                      }
                    }
                  }
                // The real EE response handler (FUN_0023c758) finishes by
                // calling the function pointer at result+0x10 == pkt+0x20.
                // Async reads pass their completion callback there: for the
                // DATA1 texture read it is entry_0021da68, which writes
                // DAT_002e24a8=1 — the exact flag FUN_0021da80 spins on after
                // submitting the read. Without this dispatch the game hangs
                // in that spin loop after every async read.
                // ONLY type 6 has a wrapper that writes +0x20 (FUN_0023cdf8
                // stores param_6 there); other types reuse a pooled packet
                // buffer, so +0x20 is stale data and must not be dispatched.
                {
                    const uint32_t cb = (workType == 6u) ? READ32(pkt + 0x20u) : 0u;
                    if (cb != 0u)
                    {
                        if (runtime->hasFunction(cb))
                        {
                            R5900Context cbCtx{};
                            cbCtx.pc = cb;
                            runtime->lookupFunction(cb)(rdram, &cbCtx, runtime);
                            PS2_IF_TRACE({ std::fprintf(stderr, "[ape-cb] async completion callback %08x\n", cb); });
                        }
                        else
                        {
                            PS2_IF_TRACE({ std::fprintf(stderr, "[ape-cb] missing callback %08x\n", cb); });
                        }
                    }
                }
                // Advance the VU1-work status token the main loop polls
                // (FUN_0023c3a8 reads 0x4011c0). On real HW the driver updates it.
                {
                    static uint32_t s_workStatus = 0u;
                    s_workStatus += 0x400u;
                    WRITE32(0x4011C0u, s_workStatus);
                }
                SET_GPR_S32(ctx, 2, 1);
                ctx->pc = getRegU32(ctx, 31);
                return;
              }
              else
              {
                const uint32_t client = READ32(pkt + 0x1Cu);
                // Per-call RPC completion callback (libsifrpc stores it in the
                // client data at +0x1C with its parameter at +0x20).  On real
                // hardware the IOP's reply raises SIF command 0x8000000C and
                // the EE-side handler runs this function; libraries such as
                // libmc use it to publish results and signal the semaphore
                // that their mcSync()/wait call blocks on.
                uint32_t completionFunction = 0u;
                uint32_t completionParameter = 0u;
                uint32_t completionRecvAddr = 0u;
                uint32_t completionParamBlock = 0u;
                bool completionUsesMcBlock = false;
                if (cmd == 0x8000000Au)
                {
                  // Diagnostic: dump RPC call packets so the loadfile
                  // request/response layout can be decoded from a live run.
                  PS2_IF_TRACE({
                  static int callDumps = 0;
                  if (callDumps < 300)
                  {
                    ++callDumps;
                    const uint32_t send = getRegU32(ctx, 7);   // $a3 srcExtra
                    const uint32_t sendSize = getRegU32(ctx, 9); // $t1 sizeExtra
                    std::fprintf(stderr,
                                 "[rpc-pkt] client=%08x rpcnum=%08x send=%08x sendSize=%x\n",
                                 client, READ32(pkt + 0x20u), send, sendSize);
                    for (uint32_t i = 0; i < 0x30 && i < sendSize; i += 16)
                    {
                      std::fprintf(stderr, "[rpc-pkt] send+%02x: %08x %08x %08x %08x\n", i,
                                   READ32(send + i), READ32(send + i + 4),
                                   READ32(send + i + 8), READ32(send + i + 12));
                    }
                  }
                  });

                  // Complete the RPC call synchronously:
                  // - loadfile client (0x3cc600, SID 0x80000006): answer module
                  //   loads with a fake non-zero module id in the recv buffer
                  //   (pkt+0x28 = recv addr, pkt+0x2C = recv size).
                  // - cdvdman and other clients: leave the recv buffer zeroed
                  //   for now.
                  const uint32_t rpcNum = READ32(pkt + 0x20u);
                  if (client == 0x003CC600u)
                  {
                    const uint32_t recvAddr = READ32(pkt + 0x28u);
                    if (recvAddr != 0u && recvAddr < 0x02000000u)
                    {
                      static uint32_t fakeModuleId = 0u;
                      ++fakeModuleId;
                      WRITE32(recvAddr, fakeModuleId);   // module id / result
                      WRITE32(recvAddr + 4u, 0);         // secondary result
                    }
                  }
                  (void)rpcNum;
                }
                if (client != 0u && (client & 3u) == 0u &&
                    client < 0x02000000u)
                {
                  if (cmd == 0x80000009u)
                  {
                    // BIND: record the client's SID so later CALLs can route
                    // into the IOP subsystem, and acknowledge the bind.
                    const uint32_t sid = READ32(pkt + 0x20u);
                    {
                      std::lock_guard<std::mutex> lock(g_clientSidMutex);
                      g_clientSids[client] = sid;
                      // The padman client (0x3c6500) is bound implicitly when
                      // the pad subsystem initializes — record it for SID
                      // 0x80000101 so pad RPCs route to the padman HLE.
                      if (sid == 0x80000101u || sid == 0x80000100u)
                      {
                        g_clientSids[0x3c6500u] = sid;
                      }
                    }
                    WRITE32(client + 0x24u, 0x0000BEEFu);
                    PS2_IF_TRACE({
                    static int bindLogs = 0;
                    if (bindLogs < 30)
                    {
                      ++bindLogs;
                      std::fprintf(stderr,
                                   "[bind] client=%08x sid=%08x canBind=%d\n",
                                   client, sid,
                                   PS2IopTransport::canBindRpc(runtime, sid) ? 1 : 0);
                    }
                    });
                  }
                  else if (cmd == 0x8000000Au)
                  {
                    // LoadModule on the loadfile client: register the module
                    // with the ps2xIOP module manager so its service routes
                    // activate (cdvdfsv etc. answer the game's later RPCs).
                    if (client == 0x003CC600u && READ32(pkt + 0x20u) == 0u)
                    {
                      const uint32_t sendAddr = getRegU32(ctx, 7);
                      if (sendAddr != 0u && sendAddr < 0x02000000u)
                      {
                        char modulePath[256];
                        uint32_t pathLen = 0u;
                        for (; pathLen < 255u; ++pathLen)
                        {
                          const char c = static_cast<char>(READ8(sendAddr + 8u + pathLen));
                          if (c == '\0')
                          {
                              break;
                          }
                          modulePath[pathLen] = c;
                        }
                        modulePath[pathLen] = '\0';
                        if (modulePath[0] != '\0')
                        {
                          const auto loadResult = runtime->loadIopModule(modulePath);
                          PS2_IF_TRACE({
                          static int loadLogs = 0;
                          if (loadLogs < 24)
                          {
                              ++loadLogs;
                              std::fprintf(stderr,
                                           "[iop-load] module='%s' id=%d\n",
                                           modulePath, loadResult.moduleId);
                          }
                          });
                        }
                      }
                    }

                    // CALL: route through the ps2xIOP subsystem so the real
                    // CDVD/file services (cdvdfsv etc.) answer the game.
                    uint32_t sid = 0u;
                    {
                      std::lock_guard<std::mutex> lock(g_clientSidMutex);
                      const auto it = g_clientSids.find(client);
                      if (it != g_clientSids.end())
                      {
                        sid = it->second;
                      }
                    }

                    ps2x::iop::RpcRequest request{};
                    request.clientAddress = client;
                    request.sid = sid;
                    request.function = READ32(pkt + 0x20u);
                    request.mode = 0u; // the game's calls block on cd+8 sema
                    request.send = {getRegU32(ctx, 7), getRegU32(ctx, 9)};   // $a3 src, $t1 size
                    request.receive = {READ32(pkt + 0x28u), READ32(pkt + 0x2Cu)};
                    request.endFunction = READ32(client + 0x1Cu);
                    request.endParameter = READ32(client + 0x20u);
                    completionFunction = request.endFunction;
                    completionParameter = request.endParameter;
                    completionRecvAddr = request.receive.address;
                    completionParamBlock = READ32(request.send.address + 0x1Cu);
                    completionUsesMcBlock = (sid == 0x80000400u || sid == 0x80000480u);

                    ps2x::iop::RpcResult iopResult =
                        PS2IopTransport::handleRpc(runtime, rdram, ctx, request);

                    // libmc drives its commands asynchronously and publishes
                    // the reply from the completion handler at 0x29BB08: the
                    // request's out-pointers are parked at 0x3C4F28/2C/30 and
                    // the semaphore mcSync() waits on is 0x2E3F10.  Some mc
                    // sends go out without that per-call callback, so replay
                    // the same completion here or mcSync() never reports the
                    // command and the card check spins forever.
                    const bool willInvokeCompletion =
                        completionFunction != 0u && completionFunction < 0x02000000u &&
                        runtime->hasFunction(completionFunction);
                    if (!willInvokeCompletion && (sid == 0x80000400u || sid == 0x80000480u))
                    {
                      // libmc's info buffer: _sceMcSendRpc copies the 0xC0-byte
                      // command block here and the IOP writes the answer back
                      // into it (type at +0, free at +4, format at +0x90).
                      const uint32_t mcInfo = 0x3C5400u;
                      const uint32_t out1 = READ32(0x3C4F28u);
                      const uint32_t out2 = READ32(0x3C4F2Cu);
                      const uint32_t out3 = READ32(0x3C4F30u);
                      if (out1 != 0u && out1 < 0x02000000u)
                        WRITE32(out1, READ32(mcInfo));
                      if (out2 != 0u && out2 < 0x02000000u)
                        WRITE32(out2, READ32(mcInfo + 4u));
                      if (out3 != 0u && out3 < 0x02000000u)
                        WRITE32(out3, READ32(mcInfo + 0x90u));
                      const uint32_t mcCompletionSema = READ32(0x2E3F10u);
                      if (mcCompletionSema != 0u && mcCompletionSema != 0xFFFFFFFFu)
                      {
                        runtime->eeScheduler().signalSemaphore(
                            static_cast<int>(mcCompletionSema), true);
                      }
                    }

                    if (!iopResult.handled && sid == 0u)
                    {
                      static int unhandledLogs = 0;
                      if (unhandledLogs < 20)
                      {
                        ++unhandledLogs;
                        std::fprintf(stderr,
                                     "[rpc] unhandled CALL client=%08x func=%08x (no SID recorded)\n",
                                     client, request.function);
                      }
                    }
                  }
                  // Both BIND and CALL unblock via the sema stored at cd+8.
                  const uint32_t sema = READ32(client + 8u);
                  if (sema != 0u && sema != 0xFFFFFFFFu)
                  {
                    runtime->eeScheduler().signalSemaphore(
                        static_cast<int>(sema), true);
                  }
                }

                // Mimic the IOP response handler: release the RPC packet back
                // to the pool.  The EE-side success paths intentionally "leak"
                // the packet because on real hardware the response processing
                // frees it; without this the 0x20-packet pool exhausts after a
                // few dozen calls and every RPC fails with -1 forever.
                WRITE32(pkt + 0x18u, 0);
                WRITE32(pkt + 0x10u, READ32(pkt + 0x10u) & 0xFFFFFFFEu);

                // Deliver the reply to the guest's completion callback, the
                // way libsifrpc does when the RPC-done command arrives.
                if (completionFunction != 0u && completionFunction < 0x02000000u &&
                    runtime->hasFunction(completionFunction))
                {
                  WRITE32(client + 0x1Cu, 0u);   // libsifrpc clears it on delivery
                  WRITE32(client + 0x20u, 0u);
                  SET_GPR_S32(ctx, 2, 1);
                  ctx->pc = getRegU32(ctx, 31);   // resume after the caller
                  GuestInvocation invocation{};
                  invocation.kind = GuestInvocationKind::RpcCallback;
                  invocation.context = *ctx;
                  invocation.context.pc = completionFunction;
                  // libsifrpc hands the reply buffer over through its uncached
                  // alias.  libmc's completion handler (0x29BB08) copies the
                  // answer out of the request's parameter block - the 0xC0-byte
                  // info buffer the IOP wrote - not the RPC's 4-byte result
                  // word, so mc clients get that block instead.
                  uint32_t replyBuffer = completionRecvAddr;
                  if (completionUsesMcBlock && completionParamBlock != 0u &&
                      completionParamBlock < 0x02000000u)
                  {
                    replyBuffer = completionParamBlock;
                  }
                  SET_GPR_U32(&invocation.context, 4, replyBuffer | 0x20000000u);
                  SET_GPR_U32(&invocation.context, 5, completionParameter);
                  SET_GPR_U32(&invocation.context, 29, runtime->eeScheduler().invocationStackTop());
                  SET_GPR_U32(&invocation.context, 31, 0u);
                  runtime->eeScheduler().invokeCurrent(std::move(invocation));
                }
              }
            }
            SET_GPR_S32(ctx, 2, 1);          // command queued successfully
            ctx->pc = getRegU32(ctx, 31);    // jr $ra
          });
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "jackie-chan-pal",
    "SCES_524.12",
    0x00110008u,
    0u,
    applyJackieChanOverrides);
