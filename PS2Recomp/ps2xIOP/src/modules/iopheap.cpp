#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // The IOP heap RPC server is provided by fileio.irx (SID 0x80000003).
        constexpr uint32_t kIopHeapSid = 0x80000003u;
        constexpr uint32_t kIopHeapBase = 0x00080000u;
        constexpr uint32_t kIopHeapLimit = 0x00200000u;
        constexpr uint32_t kIopHeapAlignment = 64u;
        constexpr uint32_t kMaxLoadHeapPath = 252u;

        class IopHeapService final : public IopService
        {
        public:
            explicit IopHeapService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "iopheap";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // fileio.irx is part of IOPRP and is always present.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return {};
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_allocations.clear();
                m_nextAddress = kIopHeapBase;
                m_unknownRpcLogCount = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kIopHeapSid)
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_callLogCount < 12u)
                    {
                        ++m_callLogCount;
                        std::ostringstream message;
                        message << "[IOPHEAP:call] rpc=0x" << std::hex << request.function
                                << " send=0x" << request.send.address << "/0x" << request.send.size
                                << " recv=0x" << request.receive.address << "/0x" << request.receive.size;
                        m_host.log(LogLevel::Info, message.str());
                    }
                }
                if (request.receive.address == 0u || request.receive.size == 0u)
                {
                    return result;
                }

                switch (request.function)
                {
                case 1:
                    handleAlloc(request);
                    return result;
                case 2:
                    handleFree(request);
                    return result;
                case 3:
                    handleLoad(request);
                    return result;
                default:
                    break;
                }

                logUnknown(request);
                const int32_t reply = 0;
                m_host.writeGuest(request.receive.address, &reply, std::min<size_t>(request.receive.size, sizeof(reply)));
                return result;
            }

        private:
            void handleAlloc(const RpcRequest &request)
            {
                uint32_t size = 0u;
                if (request.send.address == 0u || request.send.size < sizeof(size) ||
                    !m_host.readGuest(request.send.address, &size, sizeof(size)))
                {
                    writeResult(request.receive, 0u);
                    return;
                }

                uint32_t address = 0u;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const uint32_t alignedSize =
                        (size + (kIopHeapAlignment - 1u)) & ~(kIopHeapAlignment - 1u);
                    if (alignedSize != 0u && alignedSize <= kIopHeapLimit - m_nextAddress)
                    {
                        address = m_nextAddress;
                        m_nextAddress += alignedSize;
                        m_allocations.emplace(address, alignedSize);
                        if (m_allocLogCount < 16u)
                        {
                            ++m_allocLogCount;
                            shouldLog = true;
                        }
                    }
                }

                if (shouldLog)
                {
                    std::ostringstream message;
                    message << "[IOPHEAP:HLE] alloc size=0x" << std::hex << size
                            << " -> 0x" << address;
                    m_host.log(LogLevel::Info, message.str());
                }

                writeResult(request.receive, address);
            }

            void handleFree(const RpcRequest &request)
            {
                uint32_t address = 0u;
                if (request.send.address == 0u || request.send.size < sizeof(address) ||
                    !m_host.readGuest(request.send.address, &address, sizeof(address)))
                {
                    writeResult(request.receive, 0);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_allocations.erase(address);
                }
                writeResult(request.receive, 0);
            }

            void handleLoad(const RpcRequest &request)
            {
                // struct _iop_load_heap_arg { union { void *addr; int result; } p; char path[252]; }
                std::array<uint8_t, 4u + kMaxLoadHeapPath> packet{};
                const size_t packetBytes = std::min<size_t>(request.send.size, packet.size());
                if (request.send.address == 0u || packetBytes < 8u ||
                    !m_host.readGuest(request.send.address, packet.data(), packetBytes))
                {
                    writeResult(request.receive, -1);
                    return;
                }

                uint32_t destination = 0u;
                std::memcpy(&destination, packet.data(), sizeof(destination));

                const char *pathBegin = reinterpret_cast<const char *>(packet.data() + 4);
                size_t pathLength = 0u;
                while (pathLength < kMaxLoadHeapPath && pathBegin[pathLength] != '\0')
                {
                    ++pathLength;
                }

                int32_t reply = -1;
                if (destination != 0u && pathLength > 0u)
                {
                    const std::string ps2Path(pathBegin, pathLength);
                    const std::string hostPath = m_host.translateGuestPath(ps2Path);
                    const uint64_t handle = hostPath.empty() ? 0u : m_host.openHostFile(hostPath);
                    if (handle != 0u)
                    {
                        uint64_t fileSize = 0u;
                        std::vector<uint8_t> data;
                        if (m_host.hostFileSize(handle, fileSize) && fileSize > 0u &&
                            fileSize <= static_cast<uint64_t>(kIopHeapLimit - kIopHeapBase))
                        {
                            data.resize(static_cast<size_t>(fileSize));
                            size_t bytesRead = 0u;
                            if (m_host.readHostFile(handle, 0u, data.data(), data.size(), bytesRead) &&
                                bytesRead == data.size() &&
                                m_host.writeIopMemory(destination, data.data(), data.size()))
                            {
                                reply = 0;
                            }
                        }
                        m_host.closeHostFile(handle);
                    }
                }

                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_loadLogCount < 16u)
                    {
                        ++m_loadLogCount;
                        shouldLog = true;
                    }
                }

                if (shouldLog)
                {
                    std::ostringstream message;
                    message << "[IOPHEAP:HLE] load dest=0x" << std::hex << destination
                            << " path=\"" << std::string(pathBegin, pathLength) << "\""
                            << " -> " << std::dec << reply;
                    m_host.log(LogLevel::Info, message.str());
                }

                writeResult(request.receive, reply);
            }

            void writeResult(const GuestBuffer &receive, uint32_t value)
            {
                m_host.writeGuest(receive.address, &value, std::min<size_t>(receive.size, sizeof(value)));
            }

            void logUnknown(const RpcRequest &request)
            {
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_unknownRpcLogCount < 32u)
                    {
                        ++m_unknownRpcLogCount;
                        shouldLog = true;
                    }
                }

                if (!shouldLog)
                    return;

                std::ostringstream message;
                message << "[IOPHEAP:stub]"
                        << " rpc=0x" << std::hex << request.function
                        << " send=0x" << request.send.address
                        << " sendSize=0x" << request.send.size
                        << " recv=0x" << request.receive.address
                        << " recvSize=0x" << request.receive.size;
                m_host.log(LogLevel::Info, message.str());
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kIopHeapSid};

            IopHost &m_host;
            mutable std::mutex m_mutex;
            std::map<uint32_t, uint32_t> m_allocations;
            uint32_t m_nextAddress = kIopHeapBase;
            uint32_t m_unknownRpcLogCount = 0u;
            uint32_t m_allocLogCount = 0u;
            uint32_t m_loadLogCount = 0u;
            uint32_t m_callLogCount = 0u;
        };
    }

    std::unique_ptr<IopService> createIopHeapService(IopHost &host)
    {
        return std::make_unique<IopHeapService>(host);
    }
}
