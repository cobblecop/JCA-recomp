#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cctype>
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
        // Godhand uses the SCE SDK "libc" fileio client, not the ps2sdk one.
        // That client is asynchronous: each request carries a freshly created
        // EE semaphore (at +0x00) plus a result pointer (at +0x04); the server
        // performs the operation and notifies the EE, which releases the
        // semaphore.  We perform the work inline and release the semaphore
        // directly so the EE wrappers (sceOpen/sceRead/...) unblock.
        constexpr uint32_t kFileioSid = 0x80000001u;

        // RPC numbers used by the SCE SDK stdio client.
        constexpr uint32_t kFioOpen = 0u;
        constexpr uint32_t kFioClose = 1u;
        constexpr uint32_t kFioRead = 2u;
        constexpr uint32_t kFioWrite = 3u;
        constexpr uint32_t kFioLseek = 4u;
        constexpr uint32_t kFioIoctl = 5u;
        constexpr uint32_t kFioVersion = 0xffu;

        // Request packet sizes / field offsets.
        constexpr uint32_t kOpenPacketSize = 0x418u;
        constexpr uint32_t kOpenNameOffset = 0x14u;
        constexpr uint32_t kOpenNameLength = 0x400u;
        constexpr uint32_t kOpenStateIndexOffset = 0x414u;
        constexpr uint32_t kClosePacketSize = 0x14u;
        constexpr uint32_t kReadPacketSize = 0x20u;
        constexpr uint32_t kLseekPacketSize = 0x1Cu;

        constexpr uint32_t kMaxFd = 0x20u;

        constexpr uint32_t kSeekSet = 0u;
        constexpr uint32_t kSeekCur = 1u;
        constexpr uint32_t kSeekEnd = 2u;

        // Minimal ISO 9660 sector size used by the primary volume descriptor.
        constexpr uint32_t kIsoSectorSize = 2048u;

        constexpr int32_t kFioErrorUnsupported = -0x1A; // -E_LIB_UNSUPPORTED-ish
        constexpr int32_t kFioErrorNoFile = -2;         // -ENOENT

        std::string toUpperAscii(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return text;
        }

        std::string stripIsoVersion(std::string name)
        {
            const size_t version = name.find(';');
            if (version != std::string::npos)
            {
                name.resize(version);
            }
            while (!name.empty() && name.back() == '.')
            {
                name.pop_back();
            }
            return name;
        }

        std::vector<std::string> splitIsoPath(const std::string &ps2Path)
        {
            std::string normalized = toUpperAscii(ps2Path);
            const size_t colon = normalized.find(':');
            if (colon != std::string::npos)
            {
                normalized.erase(0u, colon + 1u);
            }

            std::vector<std::string> components;
            std::string current;
            for (const char c : normalized)
            {
                if (c == '\\' || c == '/')
                {
                    if (!current.empty())
                    {
                        components.push_back(stripIsoVersion(current));
                        current.clear();
                    }
                    continue;
                }
                current.push_back(c);
            }
            if (!current.empty())
            {
                components.push_back(stripIsoVersion(current));
            }
            return components;
        }

        uint32_t readLe32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        bool hasCdPrefix(std::string_view path)
        {
            std::string upper = toUpperAscii(std::string(path.substr(0u, std::min<size_t>(path.size(), 8u))));
            return upper.rfind("CDROM0:", 0u) == 0u || upper.rfind("CDROM:", 0u) == 0u;
        }

        class FileioService final : public IopService
        {
        public:
            explicit FileioService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "fileio";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // fileio.irx lives in the boot ROM / IOPRP image, so the HLE route
            // is always active.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return {};
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto &[fd, file] : m_files)
                {
                    if (file.handle != 0u)
                    {
                        m_host.closeHostFile(file.handle);
                    }
                }
                m_files.clear();
                m_callLogCount = 0u;
                m_unknownLogCount = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kFileioSid)
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;
                logCall(request);

                switch (request.function)
                {
                case kFioVersion:
                    handleVersion(request);
                    return result;
                case kFioOpen:
                    handleOpen(request);
                    return result;
                case kFioClose:
                    handleClose(request);
                    return result;
                case kFioRead:
                    handleRead(request);
                    return result;
                case kFioLseek:
                    handleLseek(request);
                    return result;
                case kFioWrite:
                    handleWrite(request);
                    return result;
                case kFioIoctl:
                    handleIoctl(request);
                    return result;
                default:
                    break;
                }

                logUnknown(request);
                // Unknown request: acknowledge and release the caller.
                const int32_t reply = 0;
                writeResult(request, reply);
                releaseRequest(request, 0u);
                return result;
            }

        private:
            struct OpenFile
            {
                uint64_t handle = 0u;   // host file handle (ISO image or host file)
                uint64_t base = 0u;     // absolute host offset of the file data
                uint64_t size = 0u;     // file size in bytes
                uint64_t offset = 0u;   // current position relative to base
            };

            void handleVersion(const RpcRequest &request)
            {
                // Godhand's loader compares the first word against the accepted
                // stdio version tags "3100" and "....".  The second word is
                // compared against 2 to select the extended completion layout.
                std::array<uint8_t, 8> reply{'3', '1', '0', '0', 0, 0, 0, 0};
                const uint32_t second = 2u;
                std::memcpy(reply.data() + 4u, &second, sizeof(second));
                m_host.writeGuest(request.receive.address, reply.data(),
                                  std::min<size_t>(request.receive.size, reply.size()));
            }

            void handleOpen(const RpcRequest &request)
            {
                std::array<uint8_t, kOpenPacketSize> packet{};
                if (!readPacket(request, packet.data(), packet.size(), 0x14u))
                {
                    writeResult(request, static_cast<int32_t>(kFioErrorNoFile));
                    releaseRequest(request, 0u);
                    return;
                }

                const uint32_t stateIndex = readLe32(packet.data() + kOpenStateIndexOffset);
                size_t nameLength = 0u;
                while (nameLength < kOpenNameLength && packet[kOpenNameOffset + nameLength] != '\0')
                {
                    ++nameLength;
                }
                const std::string name(reinterpret_cast<const char *>(packet.data() + kOpenNameOffset), nameLength);

                int32_t fd = -1;
                OpenFile file;
                bool opened = false;
                bool fromIso = false;

                if (hasCdPrefix(name) && lookupIsoFile(name, file.base, file.size))
                {
                    const std::string imagePath = m_host.hostPath(HostPathKind::CdImage);
                    if (!imagePath.empty())
                    {
                        file.handle = m_host.openHostFile(imagePath);
                        opened = file.handle != 0u;
                        fromIso = true;
                    }
                }

                if (!opened)
                {
                    const std::string hostPath = m_host.translateGuestPath(name);
                    if (!hostPath.empty())
                    {
                        file.handle = m_host.openHostFile(hostPath);
                        if (file.handle != 0u)
                        {
                            uint64_t size = 0u;
                            m_host.hostFileSize(file.handle, size);
                            file.base = 0u;
                            file.size = size;
                            file.offset = 0u;
                            opened = true;
                        }
                    }
                }

                if (opened && stateIndex < kMaxFd)
                {
                    fd = static_cast<int32_t>(stateIndex);
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_files[static_cast<uint32_t>(fd)] = file;
                }
                else if (opened)
                {
                    m_host.closeHostFile(file.handle);
                }

                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_openLogCount < 24u)
                    {
                        ++m_openLogCount;
                        shouldLog = true;
                    }
                }
                if (shouldLog)
                {
                    std::ostringstream message;
                    message << "[FILEIO:HLE] open \"" << name << "\" -> fd=" << fd
                            << (fromIso ? " (iso)" : " (host)");
                    m_host.log(LogLevel::Info, message.str());
                }

                writeResult(request, fd);
                releaseRequest(request, request.receive.address);
            }

            void handleClose(const RpcRequest &request)
            {
                std::array<uint8_t, kClosePacketSize> packet{};
                if (!readPacket(request, packet.data(), packet.size(), 0x14u))
                {
                    writeResult(request, static_cast<int32_t>(kFioErrorNoFile));
                    releaseRequest(request, 0u);
                    return;
                }

                const int32_t fd = static_cast<int32_t>(readLe32(packet.data() + 0x0Cu));
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto it = m_files.find(static_cast<uint32_t>(fd));
                    if (it != m_files.end())
                    {
                        if (it->second.handle != 0u)
                        {
                            m_host.closeHostFile(it->second.handle);
                        }
                        m_files.erase(it);
                    }
                }

                writeResult(request, static_cast<int32_t>(0));
                releaseRequest(request, request.receive.address);
            }

            void handleRead(const RpcRequest &request)
            {
                std::array<uint8_t, kReadPacketSize> packet{};
                if (!readPacket(request, packet.data(), packet.size(), 0x1Cu))
                {
                    writeResult(request, static_cast<int32_t>(kFioErrorNoFile));
                    releaseRequest(request, 0u);
                    return;
                }

                const int32_t fd = static_cast<int32_t>(readLe32(packet.data() + 0x0Cu));
                const uint32_t destAddress = readLe32(packet.data() + 0x10u);
                uint32_t size = readLe32(packet.data() + 0x14u);

                int32_t bytesRead = -1;
                if (size != 0u && destAddress != 0u)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto it = m_files.find(static_cast<uint32_t>(fd));
                    if (it != m_files.end() && it->second.handle != 0u)
                    {
                        OpenFile &file = it->second;
                        if (file.offset < file.size)
                        {
                            const uint64_t remaining = file.size - file.offset;
                            if (static_cast<uint64_t>(size) > remaining)
                            {
                                size = static_cast<uint32_t>(remaining);
                            }
                            std::vector<uint8_t> buffer(size);
                            size_t read = 0u;
                            if (m_host.readHostFile(file.handle, file.base + file.offset,
                                                    buffer.data(), buffer.size(), read) &&
                                m_host.writeGuest(destAddress, buffer.data(), read))
                            {
                                file.offset += read;
                                bytesRead = static_cast<int32_t>(read);
                            }
                        }
                        else
                        {
                            bytesRead = 0;
                        }
                    }
                }
                else
                {
                    bytesRead = 0;
                }

                writeResult(request, bytesRead);
                releaseRequest(request, request.receive.address);
            }

            void handleWrite(const RpcRequest &request)
            {
                // Godhand does not write to the disc during boot; report an
                // unsupported write and release the caller.
                writeResult(request, static_cast<int32_t>(kFioErrorUnsupported));
                releaseRequest(request, request.receive.address);
            }

            void handleLseek(const RpcRequest &request)
            {
                std::array<uint8_t, kLseekPacketSize> packet{};
                if (!readPacket(request, packet.data(), packet.size(), 0x18u))
                {
                    writeResult(request, static_cast<int32_t>(kFioErrorNoFile));
                    releaseRequest(request, 0u);
                    return;
                }

                const int32_t fd = static_cast<int32_t>(readLe32(packet.data() + 0x0Cu));
                const int32_t offset = static_cast<int32_t>(readLe32(packet.data() + 0x10u));
                const uint32_t whence = readLe32(packet.data() + 0x14u);

                int32_t newOffset = -1;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto it = m_files.find(static_cast<uint32_t>(fd));
                    if (it != m_files.end())
                    {
                        OpenFile &file = it->second;
                        int64_t target = 0;
                        switch (whence)
                        {
                        case kSeekCur:
                            target = static_cast<int64_t>(file.offset) + offset;
                            break;
                        case kSeekEnd:
                            target = static_cast<int64_t>(file.size) + offset;
                            break;
                        case kSeekSet:
                        default:
                            target = offset;
                            break;
                        }
                        if (target < 0)
                        {
                            target = 0;
                        }
                        file.offset = static_cast<uint64_t>(target);
                        newOffset = static_cast<int32_t>(file.offset);
                    }
                }

                writeResult(request, newOffset);
                releaseRequest(request, request.receive.address);
            }

            void handleIoctl(const RpcRequest &request)
            {
                writeResult(request, static_cast<int32_t>(0));
                releaseRequest(request, request.receive.address);
            }

            // ---- helpers -------------------------------------------------

            bool readPacket(const RpcRequest &request, uint8_t *buffer, size_t bufferSize, size_t minSize)
            {
                if (request.send.address == 0u || request.send.size < minSize)
                {
                    return false;
                }
                const size_t count = std::min<size_t>(request.send.size, bufferSize);
                return m_host.readGuest(request.send.address, buffer, count);
            }

            void writeResult(const RpcRequest &request, int32_t value)
            {
                if (request.receive.address != 0u && request.receive.size >= sizeof(value))
                {
                    m_host.writeGuest(request.receive.address, &value, sizeof(value));
                }
            }

            // The SCE stdio client puts a per-call EE semaphore at packet +0x00
            // and expects the server to release it when the operation is done.
            void releaseRequest(const RpcRequest &request, uint32_t semaphoreAddress)
            {
                (void)semaphoreAddress;
                uint32_t semaphore = 0u;
                if (request.send.address == 0u || request.send.size < sizeof(semaphore) ||
                    !m_host.readGuest(request.send.address, &semaphore, sizeof(semaphore)))
                {
                    return;
                }
                m_host.signalEeSemaphore(semaphore);
            }

            bool lookupIsoFile(const std::string &ps2Path, uint64_t &base, uint64_t &size)
            {
                const std::string imagePath = m_host.hostPath(HostPathKind::CdImage);
                if (imagePath.empty())
                {
                    return false;
                }
                const uint64_t handle = m_host.openHostFile(imagePath);
                if (handle == 0u)
                {
                    return false;
                }

                bool found = false;
                std::array<uint8_t, kIsoSectorSize> primaryVolume{};
                if (readHostExact(handle, 16u * kIsoSectorSize, primaryVolume.data(), primaryVolume.size()) &&
                    std::memcmp(primaryVolume.data() + 1u, "CD001", 5u) == 0)
                {
                    uint32_t dirLsn = readLe32(primaryVolume.data() + 156u + 2u);
                    uint32_t dirSize = readLe32(primaryVolume.data() + 156u + 10u);
                    const std::vector<std::string> components = splitIsoPath(ps2Path);
                    if (!components.empty() && dirSize != 0u && dirSize <= 16u * 1024u * 1024u)
                    {
                        found = true;
                        for (size_t i = 0u; i < components.size(); ++i)
                        {
                            IsoDirEntry entry;
                            if (!findIsoEntry(handle, dirLsn, dirSize, components[i], entry))
                            {
                                found = false;
                                break;
                            }
                            if (i + 1u == components.size())
                            {
                                base = static_cast<uint64_t>(entry.lsn) * kIsoSectorSize;
                                size = entry.size;
                            }
                            else
                            {
                                if ((entry.flags & 0x02u) == 0u || entry.size == 0u)
                                {
                                    found = false;
                                    break;
                                }
                                dirLsn = entry.lsn;
                                dirSize = entry.size;
                            }
                        }
                    }
                }

                m_host.closeHostFile(handle);
                return found;
            }

            struct IsoDirEntry
            {
                uint32_t lsn = 0u;
                uint32_t size = 0u;
                uint8_t flags = 0u;
            };

            bool findIsoEntry(uint64_t handle,
                              uint32_t dirLsn,
                              uint32_t dirSize,
                              const std::string &wanted,
                              IsoDirEntry &entryOut)
            {
                const std::string want = toUpperAscii(wanted);
                std::array<uint8_t, kIsoSectorSize> sector{};
                uint32_t remaining = dirSize;
                uint32_t lsn = dirLsn;
                while (remaining != 0u)
                {
                    if (!readHostExact(handle,
                                       static_cast<uint64_t>(lsn) * kIsoSectorSize,
                                       sector.data(),
                                       sector.size()))
                    {
                        return false;
                    }

                    size_t offset = 0u;
                    while (offset + 33u <= sector.size())
                    {
                        const uint8_t recordLength = sector[offset];
                        if (recordLength == 0u)
                        {
                            break;
                        }
                        if (recordLength < 33u || offset + recordLength > sector.size())
                        {
                            break;
                        }

                        const uint8_t nameLength = sector[offset + 32u];
                        if (nameLength != 0u && 33u + nameLength <= recordLength)
                        {
                            std::string name(reinterpret_cast<const char *>(sector.data() + offset + 33u),
                                             nameLength);
                            name = stripIsoVersion(name);
                            if (toUpperAscii(name) == want)
                            {
                                entryOut.lsn = readLe32(sector.data() + offset + 2u);
                                entryOut.size = readLe32(sector.data() + offset + 10u);
                                entryOut.flags = sector[offset + 25u];
                                return true;
                            }
                        }

                        offset += recordLength;
                    }

                    if (remaining <= sector.size())
                    {
                        remaining = 0u;
                    }
                    else
                    {
                        remaining -= sector.size();
                    }
                    ++lsn;
                }

                return false;
            }

            bool readHostExact(uint64_t handle, uint64_t offset, void *destination, size_t size)
            {
                size_t bytesRead = 0u;
                return m_host.readHostFile(handle, offset, destination, size, bytesRead) &&
                       bytesRead == size;
            }

            void logCall(const RpcRequest &request)
            {
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_callLogCount < 16u)
                    {
                        ++m_callLogCount;
                        shouldLog = true;
                    }
                }
                if (!shouldLog)
                {
                    return;
                }
                std::ostringstream message;
                message << "[FILEIO:call] rpc=0x" << std::hex << request.function
                        << " send=0x" << request.send.address << "/0x" << request.send.size
                        << " recv=0x" << request.receive.address << "/0x" << request.receive.size;
                m_host.log(LogLevel::Info, message.str());
            }

            void logUnknown(const RpcRequest &request)
            {
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_unknownLogCount < 16u)
                    {
                        ++m_unknownLogCount;
                        shouldLog = true;
                    }
                }
                if (!shouldLog)
                {
                    return;
                }
                std::ostringstream message;
                message << "[FILEIO:stub] rpc=0x" << std::hex << request.function
                        << " send=0x" << request.send.address << "/0x" << request.send.size;
                m_host.log(LogLevel::Info, message.str());
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kFileioSid};

            IopHost &m_host;
            mutable std::mutex m_mutex;
            std::map<uint32_t, OpenFile> m_files;
            uint32_t m_callLogCount = 0u;
            uint32_t m_unknownLogCount = 0u;
            uint32_t m_openLogCount = 0u;
        };
    }

    std::unique_ptr<IopService> createFileioService(IopHost &host)
    {
        return std::make_unique<FileioService>(host);
    }
}
