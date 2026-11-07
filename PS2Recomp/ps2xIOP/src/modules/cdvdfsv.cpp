#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // cdvdfsv registers one RPC server per CDVD command family:
        //   0x80000592 init, 0x80000593 S-commands, 0x80000595 N-commands,
        //   0x80000597 file calls, 0x8000059A/0x8000059C disk ready.
        constexpr uint32_t kCdvdInitSid = 0x80000592u;
        constexpr uint32_t kCdvdScmdSid = 0x80000593u;
        constexpr uint32_t kCdvdNcmdSid = 0x80000595u;
        constexpr uint32_t kCdvdFsvSid = 0x80000597u;
        constexpr uint32_t kCdvdDiskReadySid = 0x8000059Au;
        constexpr uint32_t kCdvdDiskReadyAltSid = 0x8000059Cu;

        constexpr uint32_t kCdvdSectorSize = 2048u;
        constexpr uint32_t kCdvdDiskTypePs2Dvd = 0x14u;
        constexpr uint32_t kCdvdDiskReadyComplete = 2u;
        constexpr uint32_t kCdvdStatusSpin = 2u;
        constexpr uint32_t kCdvdManVersion = 0x0238u;
        constexpr uint32_t kCdvdFsvVersion = 0x0238u;
        constexpr uint32_t kMaxUnknownRpcLogs = 64u;
        constexpr uint32_t kSifRpcModeNowait = 0x01u;

        uint8_t toBcd(uint32_t value)
        {
            return static_cast<uint8_t>(((value / 10u) << 4) | (value % 10u));
        }

        uint32_t readLe32(const uint8_t *bytes)
        {
            return static_cast<uint32_t>(bytes[0]) |
                   (static_cast<uint32_t>(bytes[1]) << 8) |
                   (static_cast<uint32_t>(bytes[2]) << 16) |
                   (static_cast<uint32_t>(bytes[3]) << 24);
        }

        std::string upperAscii(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::toupper(c)); });
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

        std::vector<std::string> splitCdPath(const std::string &ps2Path)
        {
            std::string normalized = upperAscii(ps2Path);
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

        struct IsoDirEntry
        {
            uint32_t lsn = 0u;
            uint32_t size = 0u;
            uint8_t flags = 0u;
            uint8_t date[7]{};
            std::string name;
        };

        class CdvdfsvService final : public IopService
        {
        public:
            explicit CdvdfsvService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "cdvdfsv";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // cdvdfsv lives in the boot ROM/IOPRP image, not in a user module,
            // so the HLE service is always active.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return {};
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_unknownRpcLogCount = 0u;
                m_readLogCount = 0u;
                m_searchLogCount = 0u;
                m_callLogCounts.clear();
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (!ownsSid(request.sid))
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;
                // cdvdfsv completes most calls synchronously. For nowait calls
                // (sceCdRead / sceCdSync) the EE side waits on the client's
                // completion semaphore, so signal it once the request is done.
                result.signalNowaitCompletion = (request.mode & kSifRpcModeNowait) != 0u;
                logCall(request);

                switch (request.sid)
                {
                case kCdvdInitSid:
                    handleInit(request);
                    return result;
                case kCdvdScmdSid:
                    handleSCmd(request);
                    return result;
                case kCdvdNcmdSid:
                    handleNCmd(request);
                    return result;
                case kCdvdFsvSid:
                    handleFsvCall(request);
                    return result;
                case kCdvdDiskReadySid:
                case kCdvdDiskReadyAltSid:
                {
                    const uint32_t ready = kCdvdDiskReadyComplete;
                    writeReply(request.receive, &ready, sizeof(ready));
                    return result;
                }
                default:
                    break;
                }

                logUnknown(request);
                const std::array<uint32_t, 2> reply{1u, 0u};
                writeWords(request.receive, reply);
                return result;
            }

        private:
            static bool ownsSid(uint32_t sid)
            {
                return std::find(kSids.begin(), kSids.end(), sid) != kSids.end();
            }

            bool writeReply(const GuestBuffer &receive, const void *data, size_t size)
            {
                if (receive.address == 0u || receive.size == 0u || size == 0u)
                    return false;
                const size_t count = std::min<size_t>(receive.size, size);
                return m_host.writeGuest(receive.address, data, count);
            }

            bool writeWords(const GuestBuffer &receive, std::span<const uint32_t> words)
            {
                return writeRpcWords(m_host, receive, words);
            }

            void handleInit(const RpcRequest &request)
            {
                // cdvdfsv_rpc1_outpacket_t
                const std::array<uint32_t, 4> reply{
                    1u,             // m_retres
                    kCdvdFsvVersion,
                    kCdvdManVersion,
                    0u, // m_debug_mode
                };
                writeWords(request.receive, reply);
            }

            void handleSCmd(const RpcRequest &request)
            {
                std::array<uint8_t, 32> reply{};
                const auto setWords = [&reply](std::initializer_list<uint32_t> values)
                {
                    size_t offset = 0;
                    for (const uint32_t value : values)
                    {
                        if (offset + sizeof(value) > reply.size())
                            break;
                        std::memcpy(reply.data() + offset, &value, sizeof(value));
                        offset += sizeof(value);
                    }
                    return offset;
                };

                size_t replySize = 0;
                switch (request.function)
                {
                case 1: // sceCdReadClock
                {
                    setWords({1u});
                    const std::time_t now = std::time(nullptr);
                    std::tm localTm{};
#ifdef _WIN32
                    localtime_s(&localTm, &now);
#else
                    localtime_r(&now, &localTm);
#endif
                    reply[4] = 0u;
                    reply[5] = toBcd(static_cast<uint32_t>(localTm.tm_sec));
                    reply[6] = toBcd(static_cast<uint32_t>(localTm.tm_min));
                    reply[7] = toBcd(static_cast<uint32_t>(localTm.tm_hour));
                    reply[8] = 0u;
                    reply[9] = toBcd(static_cast<uint32_t>(localTm.tm_mday));
                    reply[10] = toBcd(static_cast<uint32_t>(localTm.tm_mon + 1));
                    reply[11] = toBcd(static_cast<uint32_t>((localTm.tm_year + 1900) % 100));
                    replySize = 12u;
                    break;
                }
                case 3: // sceCdGetDiskType
                    replySize = setWords({kCdvdDiskTypePs2Dvd});
                    break;
                case 4: // sceCdGetError
                    replySize = setWords({0u});
                    break;
                case 5: // sceCdTrayReq (tray check / closed)
                    replySize = setWords({1u, 0u});
                    break;
                case 11: // sceCdApplySCmd
                    replySize = reply.size();
                    break;
                case 12: // sceCdStatus
                    replySize = setWords({kCdvdStatusSpin});
                    break;
                case 21: // sceCdCtrlADout
                    replySize = setWords({1u, 0u});
                    break;
                case 22: // sceCdBreak
                    replySize = setWords({1u});
                    break;
                case 26: // sceCdRM (read model id)
                    replySize = setWords({1u, 0u});
                    break;
                case 33: // sceCdPowerOff
                    replySize = setWords({1u, 0u});
                    break;
                case 34: // sceCdMmode
                    replySize = setWords({1u});
                    break;
                case 35: // sceCdChangeThreadPriority
                    replySize = setWords({1u, 0u});
                    break;
                case 37: // sceCdSetTimeout
                    replySize = setWords({1u, 0u});
                    break;
                case 39: // sceCdReadDVDdualInfo
                    replySize = setWords({1u, 0u, 0u});
                    break;
                default:
                    logUnknown(request);
                    replySize = setWords({1u, 0u});
                    break;
                }

                writeReply(request.receive, reply.data(), replySize);
            }

            void handleNCmd(const RpcRequest &request)
            {
                std::array<uint8_t, 32> reply{};
                uint32_t word = 1u;

                switch (request.function)
                {
                case 1:  // read to EE (polled)
                case 19: // read to EE (synchronous / decrypted)
                    word = readSectorsToEe(request);
                    break;
                case 4: // sceCdGetToc
                    std::memcpy(reply.data(), &word, sizeof(word));
                    word = 1u;
                    std::memcpy(reply.data() + 4, &word, sizeof(word));
                    writeReply(request.receive, reply.data(), 8u);
                    return;
                case 5:  // sceCdSeek
                case 6:  // sceCdStandby
                case 7:  // sceCdStop
                case 8:  // sceCdPause
                case 12: // sceCdApplyNCmd
                case 13: // sceCdReadIOPm
                case 15: // sceCdReadChain
                    word = 1u;
                    break;
                case 14: // sceCdDiskReady
                    word = kCdvdDiskReadyComplete;
                    break;
                default:
                    logUnknown(request);
                    word = 1u;
                    break;
                }

                writeReply(request.receive, &word, sizeof(word));
            }

            uint32_t readSectorsToEe(const RpcRequest &request)
            {
                std::array<uint32_t, 8> packet{};
                const size_t packetBytes = std::min<size_t>(request.send.size, sizeof(packet));
                if (packetBytes < sizeof(uint32_t) ||
                    !m_host.readGuest(request.send.address, packet.data(), packetBytes))
                {
                    return 0u;
                }

                const uint32_t lbn = packet[0];
                const uint32_t sectors = packet[1];
                // cdvdfsv_rpc5_01_readee semantics:
                //   m_paddr        -> EE destination for the sector payload
                //   m_eeremaindest -> EE destination for the unaligned tail/head wrapper
                //   m_eedest       -> EE destination for the running read position (u32)
                const uint32_t paddr = packet[2];
                const uint32_t eeremaindest = packet[4];
                const uint32_t eedest = packet[5];
                const uint64_t byteCount64 = static_cast<uint64_t>(sectors) * kCdvdSectorSize;

                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_readLogCount < 24u)
                    {
                        ++m_readLogCount;
                        shouldLog = true;
                    }
                }
                if (shouldLog)
                {
                    std::ostringstream message;
                    message << "[CDVDFSV:read] lbn=0x" << std::hex << lbn
                            << " sectors=0x" << sectors
                            << " paddr=0x" << paddr
                            << " eeremain=0x" << eeremaindest
                            << " eedest=0x" << eedest
                            << " bytes=0x" << byteCount64;
                    m_host.log(LogLevel::Info, message.str());
                }

                if (paddr == 0u || byteCount64 == 0u ||
                    byteCount64 > static_cast<uint64_t>(64u * 1024u * 1024u))
                {
                    if (shouldLog)
                    {
                        m_host.log(LogLevel::Warning, "[CDVDFSV:read] invalid read request");
                    }
                    return 0u;
                }

                std::vector<uint8_t> buffer(static_cast<size_t>(byteCount64), 0u);
                if (!readIsoRange(lbn, buffer.data(), buffer.size()))
                {
                    if (shouldLog)
                    {
                        m_host.log(LogLevel::Warning, "[CDVDFSV:read] ISO read failed");
                    }
                    return 0u;
                }

                // Mirror the head/body/tail split used by the real cdvdfsv so
                // unaligned EE destinations receive their 64-byte fragments in
                // the wrapper struct while the aligned body goes straight to RAM.
                const uint32_t startAddress = paddr;
                const uint32_t tailAddress = static_cast<uint32_t>(
                    (static_cast<uint64_t>(paddr) + byteCount64) & ~static_cast<uint64_t>(0x3Fu));
                const uint32_t headBytes = (paddr & 0x3Fu)
                                               ? ((paddr & ~0x3Fu) - (paddr - 0x40u))
                                               : 0u;
                const uint32_t bodyBytes = tailAddress - (paddr + headBytes);
                const uint32_t tailBytes = static_cast<uint32_t>(paddr + byteCount64 - tailAddress);

                std::array<uint8_t, 144> unaligned{};
                std::memcpy(unaligned.data() + 0u, &headBytes, sizeof(headBytes));
                std::memcpy(unaligned.data() + 4u, &tailBytes, sizeof(tailBytes));
                std::memcpy(unaligned.data() + 8u, &startAddress, sizeof(startAddress));
                std::memcpy(unaligned.data() + 12u, &tailAddress, sizeof(tailAddress));

                if (headBytes != 0u)
                {
                    std::memcpy(unaligned.data() + 16u, buffer.data(), headBytes);
                }
                if (bodyBytes != 0u &&
                    !m_host.writeGuest(paddr + headBytes, buffer.data() + headBytes, bodyBytes))
                {
                    if (shouldLog)
                    {
                        m_host.log(LogLevel::Warning, "[CDVDFSV:read] guest body write failed");
                    }
                    return 0u;
                }
                if (tailBytes != 0u)
                {
                    std::memcpy(unaligned.data() + 16u + 64u,
                                buffer.data() + headBytes + bodyBytes,
                                tailBytes);
                }

                if (eeremaindest != 0u)
                {
                    (void)m_host.writeGuest(eeremaindest, unaligned.data(), unaligned.size());
                }
                if (eedest != 0u)
                {
                    const uint32_t readPosition = static_cast<uint32_t>(buffer.size());
                    (void)m_host.writeGuest(eedest, &readPosition, sizeof(readPosition));
                }

                return static_cast<uint32_t>(buffer.size());
            }

            bool readHostExact(uint64_t handle, uint64_t offset, void *destination, size_t size)
            {
                size_t bytesRead = 0u;
                return m_host.readHostFile(handle, offset, destination, size, bytesRead) &&
                       bytesRead == size;
            }

            uint64_t openCdImage()
            {
                const std::string imagePath = m_host.hostPath(HostPathKind::CdImage);
                if (imagePath.empty())
                {
                    return 0u;
                }
                return m_host.openHostFile(imagePath);
            }

            bool readIsoRange(uint32_t lbn, void *destination, size_t size)
            {
                const uint64_t handle = openCdImage();
                if (handle == 0u)
                {
                    return false;
                }

                const bool ok = readHostExact(handle,
                                              static_cast<uint64_t>(lbn) * kCdvdSectorSize,
                                              destination,
                                              size);
                m_host.closeHostFile(handle);
                return ok;
            }

            bool findIsoEntry(uint64_t handle,
                              uint32_t dirLsn,
                              uint32_t dirSize,
                              const std::string &wanted,
                              IsoDirEntry &entryOut)
            {
                const std::string want = upperAscii(wanted);
                std::array<uint8_t, kCdvdSectorSize> sector{};
                uint32_t remaining = dirSize;
                uint32_t lsn = dirLsn;
                while (remaining != 0u)
                {
                    if (!readHostExact(handle,
                                       static_cast<uint64_t>(lsn) * kCdvdSectorSize,
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
                            if (upperAscii(name) == want)
                            {
                                entryOut.lsn = readLe32(sector.data() + offset + 2u);
                                entryOut.size = readLe32(sector.data() + offset + 10u);
                                entryOut.flags = sector[offset + 25u];
                                std::memcpy(entryOut.date, sector.data() + offset + 18u, sizeof(entryOut.date));
                                entryOut.name = name;
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

            bool searchIsoFile(const std::string &ps2Path, IsoDirEntry &entryOut)
            {
                const uint64_t handle = openCdImage();
                if (handle == 0u)
                {
                    return false;
                }

                bool found = false;
                std::array<uint8_t, kCdvdSectorSize> primaryVolume{};
                if (readHostExact(handle, 16u * kCdvdSectorSize, primaryVolume.data(), primaryVolume.size()) &&
                    std::memcmp(primaryVolume.data() + 1u, "CD001", 5u) == 0)
                {
                    uint32_t dirLsn = readLe32(primaryVolume.data() + 156u + 2u);
                    uint32_t dirSize = readLe32(primaryVolume.data() + 156u + 10u);
                    const std::vector<std::string> components = splitCdPath(ps2Path);
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
                                entryOut = entry;
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

            void handleFsvCall(const RpcRequest &request)
            {
                // The search-file packet is one of three libcdvd layouts:
                //   0x12c / 0x128: sceCdlFILE, attributes, path[256], eedest, layer
                //   0x124:         sceCdlFILE, path[256], eedest
                const uint32_t packetSize = request.send.size;
                if (request.function == 0u &&
                    (packetSize == 0x124u || packetSize == 0x128u || packetSize == 0x12Cu))
                {
                    const bool hasAttributes = packetSize != 0x124u;
                    const size_t pathOffset = hasAttributes ? 36u : 32u;
                    const size_t eedestOffset = hasAttributes ? 292u : 288u;
                    std::array<uint8_t, 300> packet{};
                    const size_t packetBytes = std::min<size_t>(packetSize, packet.size());
                    if (request.send.address != 0u &&
                        m_host.readGuest(request.send.address, packet.data(), packetBytes))
                    {
                        std::string ps2Path(reinterpret_cast<const char *>(packet.data() + pathOffset), 256u);
                        const size_t terminator = ps2Path.find('\0');
                        if (terminator != std::string::npos)
                        {
                            ps2Path.resize(terminator);
                        }

                        uint32_t eedest = 0u;
                        if (packetBytes >= eedestOffset + sizeof(eedest))
                        {
                            std::memcpy(&eedest, packet.data() + eedestOffset, sizeof(eedest));
                        }

                        IsoDirEntry entry;
                        if (!ps2Path.empty() && searchIsoFile(ps2Path, entry))
                        {
                            struct CdlFile
                            {
                                uint32_t lsn;
                                uint32_t size;
                                char name[16];
                                uint8_t date[8];
                            } file{};
                            file.lsn = entry.lsn;
                            file.size = entry.size;
                            const size_t nameLength = std::min<size_t>(15u, entry.name.size());
                            std::memcpy(file.name, entry.name.data(), nameLength);
                            std::memcpy(file.date, entry.date, 7u);

                            std::array<uint8_t, 36> payload{};
                            std::memcpy(payload.data(), &file, sizeof(file));
                            const uint32_t attributes = 0u;
                            std::memcpy(payload.data() + sizeof(file), &attributes, sizeof(attributes));

                            const uint32_t outputAddress = eedest != 0u ? eedest : request.send.address;
                            const size_t outputSize = hasAttributes ? 36u : 32u;
                            (void)m_host.writeGuest(outputAddress, payload.data(), outputSize);

                            // Temporary diagnostic: read back what the guest sees.
                            {
                                uint8_t verify[32]{};
                                if (m_host.readGuest(outputAddress, verify, sizeof(verify)))
                                {
                                    uint32_t vlsn = 0u;
                                    uint32_t vsize = 0u;
                                    std::memcpy(&vlsn, verify, 4u);
                                    std::memcpy(&vsize, verify + 4u, 4u);
                                    char vname[17]{};
                                    std::memcpy(vname, verify + 8u, 16u);
                                    std::ostringstream m;
                                    m << "[CDVDFSV:search-write] out=0x" << std::hex
                                      << outputAddress << " eedest=0x" << eedest
                                      << " send=0x" << request.send.address
                                      << " lsn=0x" << vlsn << " size=0x" << vsize
                                      << std::dec << " name=\"" << vname << "\"";
                                    m_host.log(LogLevel::Info, m.str());
                                }
                            }

                            const uint32_t reply = 1u;
                            writeReply(request.receive, &reply, sizeof(reply));
                            logSearch(ps2Path, entry, true);
                            return;
                        }

                        const uint32_t reply = 0u;
                        writeReply(request.receive, &reply, sizeof(reply));
                        logSearch(ps2Path, entry, false);
                        return;
                    }
                }

                logUnknown(request);
                const uint32_t reply = 1u;
                writeReply(request.receive, &reply, sizeof(reply));
            }

            void logSearch(const std::string &ps2Path, const IsoDirEntry &entry, bool found)
            {
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_searchLogCount < 24u)
                    {
                        ++m_searchLogCount;
                        shouldLog = true;
                    }
                }

                if (!shouldLog)
                    return;

                std::ostringstream message;
                message << "[CDVDFSV:search] path=\"" << ps2Path << "\"";
                if (found)
                {
                    message << " lsn=0x" << std::hex << entry.lsn
                            << " size=0x" << entry.size
                            << " name=\"" << entry.name << "\"";
                }
                else
                {
                    message << " not found";
                }
                m_host.log(LogLevel::Info, message.str());
            }

            void logCall(const RpcRequest &request)
            {
                const uint64_t key = (static_cast<uint64_t>(request.sid) << 32) |
                                     static_cast<uint64_t>(request.function);
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    uint32_t &count = m_callLogCounts[key];
                    if (count < 3u)
                    {
                        ++count;
                        shouldLog = true;
                    }
                }

                if (!shouldLog)
                    return;

                uint32_t firstWord = 0u;
                if (request.send.address != 0u && request.send.size >= sizeof(firstWord))
                {
                    (void)m_host.readGuest(request.send.address, &firstWord, sizeof(firstWord));
                }

                std::ostringstream message;
                message << "[CDVDFSV:call]"
                        << " sid=0x" << std::hex << request.sid
                        << " rpc=0x" << request.function
                        << " mode=0x" << request.mode
                        << " send=0x" << request.send.address
                        << "/0x" << request.send.size
                        << " recv=0x" << request.receive.address
                        << "/0x" << request.receive.size
                        << " w0=0x" << firstWord;
                m_host.log(LogLevel::Info, message.str());
            }

            void logUnknown(const RpcRequest &request)
            {
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_unknownRpcLogCount < kMaxUnknownRpcLogs)
                    {
                        ++m_unknownRpcLogCount;
                        shouldLog = true;
                    }
                }

                if (!shouldLog)
                    return;

                std::ostringstream message;
                message << "[CDVDFSV:stub]"
                        << " sid=0x" << std::hex << request.sid
                        << " rpc=0x" << request.function
                        << " send=0x" << request.send.address
                        << " sendSize=0x" << request.send.size
                        << " recv=0x" << request.receive.address
                        << " recvSize=0x" << request.receive.size;
                m_host.log(LogLevel::Info, message.str());
            }

            inline static constexpr std::array<uint32_t, 6> kSids{
                kCdvdInitSid,
                kCdvdScmdSid,
                kCdvdNcmdSid,
                kCdvdFsvSid,
                kCdvdDiskReadySid,
                kCdvdDiskReadyAltSid,
            };

            IopHost &m_host;
            mutable std::mutex m_mutex;
            uint32_t m_unknownRpcLogCount = 0u;
            uint32_t m_readLogCount = 0u;
            uint32_t m_searchLogCount = 0u;
            std::unordered_map<uint64_t, uint32_t> m_callLogCounts;
        };
    }

    std::unique_ptr<IopService> createCdvdfsvService(IopHost &host)
    {
        return std::make_unique<CdvdfsvService>(host);
    }
}
