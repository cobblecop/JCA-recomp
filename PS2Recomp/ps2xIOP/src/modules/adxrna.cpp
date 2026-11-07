#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <span>
#include <string_view>

namespace ps2x::iop::detail
{
    namespace
    {
        // Godhand's PS2RNA/ADXRNA audio middleware binds a client to the CRI
        // ADXI IOP server before it starts streaming.  The real module lives
        // in MODULES\CRI_ADXI.IRX (or is embedded in the game's IOPRP image)
        // and is not executed by the HLE IOP, so the server is emulated here.
        //
        // The boot path only requires bind to succeed and the stream-create
        // command (function 2) to return a non-zero handle.  Everything else
        // is acknowledged with a zero status; audio output is not implemented
        // yet, but the guest keeps running instead of spinning in its bind
        // retry loop.
        constexpr uint32_t kAdxRnaSid = 0x90000200u;

        constexpr uint32_t kCmdCreateStream = 2u;
        constexpr uint32_t kCmdDestroyStream = 3u;
        // PS2RNA/ADX commands that return a required non-zero remote handle:
        //  - 0x400: create a stream/port object (FUN_0033B428)
        //  - 0x422: SJUNI_CreateRmt (FUN_0033AED0)
        constexpr uint32_t kCmdCreateRemote0 = 0x400u;
        constexpr uint32_t kCmdCreateSjuniv = 0x422u;

        class AdxRnaService final : public IopService
        {
        public:
            explicit AdxRnaService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "adxrna";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                // Always active: the CRI module may be loaded through SMOD
                // from the packed IRX.IMS container, which does not report
                // individual module names back to the HLE module manager.
                return {};
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_nextHandle = 1u;
                m_callLogCount = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kAdxRnaSid)
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;

                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_callLogCount < 32u)
                {
                    ++m_callLogCount;
                    std::ostringstream message;
                    message << "[ADXRNA:call] rpc=0x" << std::hex << request.function
                            << " mode=0x" << request.mode
                            << " send=0x" << request.send.address << "/0x" << request.send.size
                            << " recv=0x" << request.receive.address << "/0x" << request.receive.size;
                    m_host.log(LogLevel::Info, message.str());
                }

                if (request.function == kCmdCreateStream ||
                    request.function == kCmdCreateRemote0 ||
                    request.function == kCmdCreateSjuniv)
                {
                    // Return a non-zero opaque stream handle.  PS2RNA checks
                    // this and aborts into an infinite error loop if it is 0.
                    const uint32_t handle = m_nextHandle++;
                    (void)writeRpcWords(m_host, request.receive, std::span<const uint32_t>(&handle, 1));
                    return result;
                }

                if (request.function == kCmdDestroyStream)
                {
                    const uint32_t status = 0u;
                    (void)writeRpcWords(m_host, request.receive, std::span<const uint32_t>(&status, 1));
                    return result;
                }

                // Generic command: report success (0) in the first reply word
                // and clear the rest so callers do not observe stale data.
                if (request.receive.address != 0u && request.receive.size != 0u)
                {
                    const uint32_t zero = 0u;
                    m_host.writeGuest(request.receive.address,
                                      &zero,
                                      std::min<size_t>(request.receive.size, sizeof(zero)));
                    if (request.receive.size > sizeof(zero))
                    {
                        m_host.zeroGuest(request.receive.address + sizeof(zero),
                                         request.receive.size - sizeof(zero));
                    }
                }
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{kAdxRnaSid};

            IopHost &m_host;
            std::mutex m_mutex;
            uint32_t m_nextHandle = 1u;
            uint32_t m_callLogCount = 0u;
        };
    }

    std::unique_ptr<IopService> createAdxRnaService(IopHost &host)
    {
        return std::make_unique<AdxRnaService>(host);
    }
}
