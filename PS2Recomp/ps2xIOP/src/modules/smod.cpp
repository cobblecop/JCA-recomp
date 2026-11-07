#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <span>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // The EE-side module loader talks to the boot ROM / IOPRP "smod"
        // service on SID 0x80000006.  Godhand uses it to load the IRX modules
        // packed inside MODULES\IRX.IMS.
        constexpr uint32_t kSmodSid = 0x80000006u;

        constexpr uint32_t kSmodRpcLoadModule = 6u;
        constexpr uint32_t kSmodRpcVersion = 0xffu;

        class SmodService final : public IopService
        {
        public:
            explicit SmodService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "smod";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // smod is part of the boot ROM / IOPRP image, so the HLE route is
            // always active.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return {};
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_nextModuleId = 1u;
                m_callLogCount = 0u;
                m_unhandledLogCount = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kSmodSid)
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
                    message << "[SMOD:call] rpc=0x" << std::hex << request.function
                            << " send=0x" << request.send.address << "/0x" << request.send.size
                            << " recv=0x" << request.receive.address << "/0x" << request.receive.size;
                    m_host.log(LogLevel::Info, message.str());
                }
                switch (request.function)
                {
                case kSmodRpcVersion:
                {
                    // Godhand's loader compares this 4-byte string against the
                    // accepted loadcore/smod version tags "3100" and "....".
                    // Report the SDK 3.1.0 tag so the compatibility check passes.
                    constexpr std::array<char, 4> version{'3', '1', '0', '0'};
                    writeReply(request.receive, version.data(), version.size());
                    break;
                }
                case kSmodRpcLoadModule:
                {
                    // The runtime services modules through HLE rather than
                    // executing the IRX, so acknowledge the load and hand back
                    // a synthetic module id.
                    const std::array<uint32_t, 2> reply{m_nextModuleId++, 0u};
                    writeWords(request.receive, reply);
                    break;
                }
                default:
                {
                    logUnhandled(request);
                    const uint32_t ok = 0u;
                    writeReply(request.receive, &ok, sizeof(ok));
                    break;
                }
                }

                return result;
            }

        private:
            bool writeReply(const GuestBuffer &receive, const void *data, size_t size)
            {
                if (receive.address == 0u || receive.size == 0u || size == 0u)
                {
                    return false;
                }
                const size_t count = std::min<size_t>(receive.size, size);
                return m_host.writeGuest(receive.address, data, count);
            }

            bool writeWords(const GuestBuffer &receive, std::span<const uint32_t> words)
            {
                return writeRpcWords(m_host, receive, words);
            }

            void logUnhandled(const RpcRequest &request)
            {
                if (m_unhandledLogCount++ >= 8u)
                {
                    return;
                }
                std::ostringstream message;
                message << "[" << name() << "] unhandled rpc=0x" << std::hex << request.function
                        << " sid=0x" << request.sid;
                m_host.log(LogLevel::Info, message.str());
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kSmodSid};

            IopHost &m_host;
            std::mutex m_mutex;
            uint32_t m_nextModuleId = 1u;
            uint32_t m_unhandledLogCount = 0u;
            uint32_t m_callLogCount = 0u;
        };
    }

    std::unique_ptr<IopService> createSmodService(IopHost &host)
    {
        return std::make_unique<SmodService>(host);
    }
}
