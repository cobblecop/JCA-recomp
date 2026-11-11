#include "ps2recomp/map_emitter.h"

#include "ps2recomp/elf_parser.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace ps2recomp
{
    namespace
    {
        bool isConditionalBranch(uint32_t opcode)
        {
            switch (opcode)
            {
            case 0x01: // REGIMM: bltz, bgez, bltzal, bgezal ...
            case 0x04: // beq
            case 0x05: // bne
            case 0x06: // blez
            case 0x07: // bgtz
            case 0x14: // beql
            case 0x15: // bnel
            case 0x16: // blezl
            case 0x17: // bgtzl
                return true;
            default:
                return false;
            }
        }

        int32_t branchOffset(uint32_t word)
        {
            return static_cast<int16_t>(word & 0xFFFF);
        }

        uint32_t jumpTarget(uint32_t pc, uint32_t word)
        {
            return ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
        }
    }

    MapEmitter::MapEmitter(ElfParser &parser)
        : m_parser(parser)
    {
    }

    bool MapEmitter::wordAt(uint32_t address, uint32_t &word) const
    {
        if (!m_parser.isValidAddress(address))
        {
            return false;
        }
        try
        {
            word = m_parser.readWord(address);
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    MapEntry MapEmitter::reach(uint32_t start, uint32_t lo, uint32_t hi) const
    {
        std::vector<uint32_t> work;
        std::unordered_set<uint32_t> seen;
        work.push_back(start);

        uint32_t end = start;
        while (!work.empty())
        {
            uint32_t pc = work.back();
            work.pop_back();

            while (pc >= lo && pc < hi && seen.find(pc) == seen.end())
            {
                uint32_t word = 0;
                if (!wordAt(pc, word))
                {
                    break;
                }

                seen.insert(pc);
                end = std::max(end, pc + 4);

                const uint32_t opcode = word >> 26;
                if (opcode == 0x00)
                {
                    const uint32_t function = word & 0x3F;
                    if (function == 8 || function == 9) // jr / jalr
                    {
                        end = std::max(end, pc + 8); // the delay slot still runs
                        break;
                    }
                    pc += 4;
                }
                else if (opcode == 0x02) // j
                {
                    const uint32_t target = jumpTarget(pc, word);
                    end = std::max(end, pc + 8);
                    if (target >= lo && target < hi)
                    {
                        work.push_back(target);
                    }
                    break;
                }
                else if (opcode == 0x03) // jal
                {
                    const uint32_t target = jumpTarget(pc, word);
                    if (target >= lo && target < hi)
                    {
                        work.push_back(target);
                    }
                    pc += 4;
                }
                else if (isConditionalBranch(opcode))
                {
                    const int64_t target = static_cast<int64_t>(pc) + 4 + (static_cast<int64_t>(branchOffset(word)) << 2);
                    if (target >= static_cast<int64_t>(lo) && target < static_cast<int64_t>(hi))
                    {
                        work.push_back(static_cast<uint32_t>(target));
                    }
                    pc += 4;
                }
                else
                {
                    pc += 4;
                }
            }
        }

        return MapEntry{start, std::max(end, start + 4)};
    }

    void MapEmitter::collectSeeds(uint32_t lo, uint32_t hi, std::vector<uint32_t> &out) const
    {
        const std::vector<Section> sections = const_cast<ElfParser &>(m_parser).getSections();

        // Every aligned word anywhere in the image whose value lands in the code
        // range: that is how dispatch tables, vtables and inline jump tables
        // reference code.
        for (const Section &section : sections)
        {
            if (section.data == nullptr || section.size == 0)
            {
                continue;
            }
            for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4)
            {
                uint32_t value = 0;
                std::memcpy(&value, section.data + offset, sizeof(value));
                // Section contents are little-endian R5900 code and data.
                if (value >= lo && value < hi && (value & 3) == 0)
                {
                    out.push_back(value);
                }
            }
        }

        // lui/addiu and lui/ori pairs that materialise a code address.
        for (const Section &section : sections)
        {
            if (!section.isCode)
            {
                continue;
            }
            const uint32_t begin = std::max(section.address, lo);
            const uint32_t end = std::min(section.address + section.size, hi);
            uint32_t high[32] = {0};
            bool highValid[32] = {false};
            for (uint32_t pc = begin; pc + 4 <= end; pc += 4)
            {
                uint32_t word = 0;
                if (!wordAt(pc, word))
                {
                    continue;
                }
                const uint32_t opcode = word >> 26;
                const uint32_t rs = (word >> 21) & 31;
                const uint32_t rt = (word >> 16) & 31;

                if (opcode == 0x0F) // lui
                {
                    high[rt] = (word & 0xFFFF) << 16;
                    highValid[rt] = true;
                }
                else if ((opcode == 0x09 || opcode == 0x0D) && highValid[rs]) // addiu / ori
                {
                    const uint32_t value = high[rs] + (word & 0xFFFF);
                    if (value >= lo && value < hi && (value & 3) == 0)
                    {
                        out.push_back(value);
                    }
                }
            }
        }
    }

    std::vector<MapEntry> MapEmitter::build(const std::vector<uint32_t> &extraStarts, bool useSeeds) const
    {
        std::vector<MapEntry> entries;
        const std::vector<Section> sections = const_cast<ElfParser &>(m_parser).getSections();

        for (const Section &section : sections)
        {
            if (!section.isCode || section.size == 0)
            {
                continue;
            }

            const uint32_t lo = section.address;
            const uint32_t hi = section.address + section.size;

            std::vector<uint32_t> seeds = extraStarts;
            if (useSeeds)
            {
                collectSeeds(lo, hi, seeds);
            }
            std::sort(seeds.begin(), seeds.end());
            seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
            seeds.erase(std::remove_if(seeds.begin(), seeds.end(),
                                       [lo, hi](uint32_t a)
                                       { return a < lo || a >= hi; }),
                        seeds.end());

            uint32_t start = lo;
            while (start < hi)
            {
                const MapEntry extent = reach(start, lo, hi);
                if (extent.end <= extent.start)
                {
                    start += 4;
                    continue;
                }

                uint32_t cursor = extent.start;
                auto it = std::upper_bound(seeds.begin(), seeds.end(), cursor);
                for (; it != seeds.end() && *it < extent.end; ++it)
                {
                    if (*it - cursor >= 8)
                    {
                        entries.push_back(MapEntry{cursor, *it});
                        cursor = *it;
                    }
                }
                entries.push_back(MapEntry{cursor, extent.end});
                start = extent.end;
            }
        }

        return entries;
    }

    bool MapEmitter::writeCsv(const std::string &path, const std::vector<MapEntry> &entries)
    {
        std::ofstream out(path);
        if (!out)
        {
            return false;
        }

        out << "Name,Start,End,Size\n";
        char name[64];
        char startText[16];
        char endText[16];
        for (const MapEntry &entry : entries)
        {
            std::snprintf(name, sizeof(name), "entry_gap_%08x", entry.start);
            std::snprintf(startText, sizeof(startText), "0x%08X", entry.start);
            std::snprintf(endText, sizeof(endText), "0x%08X", entry.end);
            out << name << ',' << startText << ',' << endText << ',' << (entry.end - entry.start) << '\n';
        }
        return static_cast<bool>(out);
    }

    std::vector<uint32_t> MapEmitter::readAddresses(const std::string &path)
    {
        std::vector<uint32_t> addresses;
        std::ifstream in(path);
        if (!in)
        {
            return addresses;
        }

        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string text = buffer.str();

        // Comments carry example addresses that are not real code.
        text = std::regex_replace(text, std::regex("/\\*.*?\\*/", std::regex::extended), " ");
        text = std::regex_replace(text, std::regex("//[^\\n]*"), " ");

        const std::regex literal("0x([0-9a-fA-F]{6,8})");
        for (std::sregex_iterator it(text.begin(), text.end(), literal), end; it != end; ++it)
        {
            addresses.push_back(static_cast<uint32_t>(std::stoul((*it)[1].str(), nullptr, 16)));
        }
        return addresses;
    }
}
