#ifndef PS2RECOMP_MAP_EMITTER_H
#define PS2RECOMP_MAP_EMITTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace ps2recomp
{
    class ElfParser;

    struct MapEntry
    {
        uint32_t start = 0;
        uint32_t end = 0;
    };

    // Discovers function extents from the executable alone, so a map can be
    // produced without a Ghidra export.
    //
    // Boundaries come from recursive descent rather than "split at the first
    // return": the walk follows branch and jump targets, and a path only ends
    // at jr/jalr/j.  A function with an early return (the C runtime boot stub
    // has several) therefore stays whole, which a linear sweep gets wrong.
    class MapEmitter
    {
    public:
        explicit MapEmitter(ElfParser &parser);

        // Sweeps every code section.  extraStarts are forced entry points
        // (addresses the runtime binds, for example).  useSeeds adds entry
        // points a descent cannot see: pointer values in data, and
        // lui/addiu|ori pairs that materialise an address in code.
        std::vector<MapEntry> build(const std::vector<uint32_t> &extraStarts,
                                    bool useSeeds = true) const;

        static bool writeCsv(const std::string &path, const std::vector<MapEntry> &entries);

        // Hex addresses in a source or config file, with comments removed.
        static std::vector<uint32_t> readAddresses(const std::string &path);

    private:
        bool wordAt(uint32_t address, uint32_t &word) const;
        MapEntry reach(uint32_t start, uint32_t lo, uint32_t hi) const;
        void collectSeeds(uint32_t lo, uint32_t hi, std::vector<uint32_t> &out) const;

        ElfParser &m_parser;
    };
}

#endif // PS2RECOMP_MAP_EMITTER_H
