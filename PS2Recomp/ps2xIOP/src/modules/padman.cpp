#include "module_factories.h"
#include "rpc_reply.h"

#include "ps2x/iop/pad_dma.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <sstream>
#include <string_view>

namespace ps2x::iop::detail
{
    namespace
    {
        // God Hand's statically linked libpad binds both of padman's RPC ids
        // before it performs the optional module-version check.  PADMAN.IRX
        // is loaded from MODULES\IRX.IMS by SMOD, which does not report the
        // individual module name back to the HLE module manager, so this
        // service is always active.
        constexpr uint32_t kPadBindId1New = 0x80000100u;
        constexpr uint32_t kPadBindId2New = 0x80000101u;

        // libpad command numbers (the padman RPC always uses function 1).
        constexpr uint32_t kCmdOpen = 0x01u;
        constexpr uint32_t kCmdSetMainMode = 0x06u;
        constexpr uint32_t kCmdSetActDir = 0x07u;
        constexpr uint32_t kCmdSetActAlign = 0x08u;
        constexpr uint32_t kCmdGetButtonMask = 0x09u;
        constexpr uint32_t kCmdSetButtonInfo = 0x0Au;
        constexpr uint32_t kCmdSetVref = 0x0Bu;
        constexpr uint32_t kCmdGetPortMax = 0x0Cu;
        constexpr uint32_t kCmdGetSlotMax = 0x0Du;
        constexpr uint32_t kCmdClose = 0x0Eu;
        constexpr uint32_t kCmdEnd = 0x0Fu;
        constexpr uint32_t kCmdInit = 0x10u;
        constexpr uint32_t kCmdGetModVersion = 0x12u;

        // Report padman 4.0.  The game's libpad version check compares the
        // upper byte against 4 and aborts the boot into a busy-wait loop when
        // it does not match.
        constexpr uint32_t kPadModuleVersion = 0x0400u;

        class PadManService final : public IopService
        {
        public:
            explicit PadManService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "padman";
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
                padDmaReset();
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != kPadBindId1New && request.sid != kPadBindId2New)
                {
                    return {};
                }

                RpcResult result;
                result.handled = true;
                result.resultAddress = request.receive.address;

                std::lock_guard<std::mutex> lock(m_mutex);

                uint32_t command = 0u;
                if (request.send.address != 0u && request.send.size >= sizeof(command))
                {
                    (void)m_host.readGuest(request.send.address, &command, sizeof(command));
                }

                if (m_callLogCount < 24u)
                {
                    ++m_callLogCount;
                    std::ostringstream message;
                    message << "[PADMAN:call] sid=0x" << std::hex << request.sid
                            << " rpc=0x" << request.function
                            << " cmd=0x" << command
                            << " mode=0x" << request.mode
                            << " send=0x" << request.send.address << "/0x" << request.send.size
                            << " recv=0x" << request.receive.address << "/0x" << request.receive.size;
                    m_host.log(LogLevel::Info, message.str());
                }

                if ((command == kCmdOpen || command == kCmdClose) &&
                    request.send.address != 0u && request.send.size >= 0x14u)
                {
                    // libpad passes the 0x100-byte status area it expects
                    // PADMAN to DMA pad frames into.
                    std::array<uint32_t, 5> args{};
                    (void)m_host.readGuest(request.send.address, args.data(), sizeof(args));
                    const int port = static_cast<int>(args[1]);
                    const int slot = static_cast<int>(args[2]);
                    if (command == kCmdOpen)
                    {
                        padDmaSetArea(port, slot, args[4]);
                    }
                    else
                    {
                        padDmaClearArea(port, slot);
                    }
                }

                uint32_t reply = 0u;
                switch (command)
                {
                case kCmdGetModVersion:
                    reply = kPadModuleVersion;
                    break;
                case kCmdGetPortMax:
                    // libpad keeps state for two controller ports.
                    reply = 2u;
                    break;
                case kCmdGetSlotMax:
                    // libpad keeps state for eight slots per port.
                    reply = 8u;
                    break;
                case kCmdGetButtonMask:
                    // Standard (non-pressure) controller button mask.
                    reply = 0xFFFFu;
                    break;
                case kCmdOpen:
                case kCmdClose:
                case kCmdEnd:
                case kCmdInit:
                case kCmdSetMainMode:
                case kCmdSetActDir:
                case kCmdSetActAlign:
                case kCmdSetButtonInfo:
                case kCmdSetVref:
                    reply = 1u;
                    break;
                default:
                    reply = 0u;
                    break;
                }

                if (request.receive.address != 0u && request.receive.size != 0u)
                {
                    (void)m_host.zeroGuest(request.receive.address, request.receive.size);
                    // libpad reads the command result from offset 0x0C of the
                    // shared 0x80-byte request/reply buffer.
                    const size_t replyOffset = request.receive.size >= 0x10u ? 0x0Cu : 0u;
                    (void)writeRpcWords(m_host,
                                        {request.receive.address + static_cast<uint32_t>(replyOffset),
                                         request.receive.size - replyOffset},
                                        std::span<const uint32_t>(&reply, 1));
                }
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 2> kSids{kPadBindId1New, kPadBindId2New};

            IopHost &m_host;
            std::mutex m_mutex;
            uint32_t m_callLogCount = 0u;
        };
    }

    std::unique_ptr<IopService> createPadManService(IopHost &host)
    {
        return std::make_unique<PadManService>(host);
    }
}
