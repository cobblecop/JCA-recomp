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
        // Godhand loads LIBSD/SDRDRV/SDHD/SNDDRV from MODULES\IRX.IMS and then
        // binds its sound-driver RPC server.  The module is not executed by the
        // HLE IOP, so acknowledge its commands here.
        constexpr uint32_t kSndDrvSid = 0x19720317u;

        class SndDrvService final : public IopService
        {
        public:
            explicit SndDrvService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "snddrv";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return {};
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_callLogCount = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kSndDrvSid)
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;

                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_callLogCount < 12u)
                {
                    ++m_callLogCount;
                    std::ostringstream message;
                    message << "[SNDDRV:call] rpc=0x" << std::hex << request.function
                            << " mode=0x" << request.mode
                            << " send=0x" << request.send.address << "/0x" << request.send.size
                            << " recv=0x" << request.receive.address << "/0x" << request.receive.size;
                    m_host.log(LogLevel::Info, message.str());
                }

                // Every SNDDRV command used during boot has a 0x40-byte reply
                // buffer; zero it (success / idle) and report a zero status.
                if (request.receive.address != 0u && request.receive.size != 0u)
                {
                    const uint32_t zero = 0u;
                    m_host.writeGuest(request.receive.address,
                                      &zero,
                                      std::min<size_t>(request.receive.size, sizeof(zero)));
                }
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{kSndDrvSid};

            IopHost &m_host;
            std::mutex m_mutex;
            uint32_t m_callLogCount = 0u;
        };
    }

    std::unique_ptr<IopService> createSndDrvService(IopHost &host)
    {
        return std::make_unique<SndDrvService>(host);
    }
}
