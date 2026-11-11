#include "ps2recomp/elf_analyzer.h"
#include "ps2recomp/elf_parser.h"
#include "ps2recomp/map_emitter.h"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

void printUsage()
{
    std::cout << "PS2 ELF Analyzer\n";
    std::cout << "A tool to analyze PS2 ELF files and generate TOML configuration for PS2Recomp\n\n";
    std::cout << "Usage: ps2_analyzer <input_elf> <output_toml> [sce_symbol_db_dir]\n"
                 "                     [--emit-map <out.csv>] [--seeds-from <file> ...]\n"
                 "  --emit-map     Also discover function extents and write a map CSV\n"
                 "                 (the format a config's ghidra_output consumes)\n"
                 "  --seeds-from   Force the hex addresses in <file> to be entry points\n";
    std::cout << "  input_elf    Path to the PS2 ELF file\n";
    std::cout << "  output_toml  Path to output TOML configuration file\n";
    std::cout << "  sce_symbol_db_dir  Optional override directory containing symbols.json and tree.json\n";
    std::cout << "                     If omitted, the embedded SCE symbol database is used\n";
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printUsage();
        return 1;
    }

    std::string elfPath = argv[1];
    std::string tomlPath = argv[2];
    std::string sceSymbolDbPath;
    std::string emitMapPath;
    std::vector<std::string> seedFiles;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--emit-map") == 0 && i + 1 < argc)
        {
            emitMapPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--seeds-from") == 0 && i + 1 < argc)
        {
            seedFiles.emplace_back(argv[++i]);
        }
        else if (sceSymbolDbPath.empty())
        {
            sceSymbolDbPath = argv[i];
        }
    }

    std::cout << "PS2 ELF Analyzer\n";
    std::cout << "----------------\n";
    std::cout << "Input ELF: " << elfPath << "\n";
    std::cout << "Output TOML: " << tomlPath << "\n\n";
    if (!sceSymbolDbPath.empty())
    {
        std::cout << "SCE symbol DB: " << sceSymbolDbPath << "\n\n";
    }
    else
    {
        std::cout << "SCE symbol DB: embedded\n\n";
    }

    try
    {
        ps2recomp::ElfAnalyzer analyzer(elfPath);
        if (!sceSymbolDbPath.empty())
        {
            analyzer.setSceSymbolDatabasePath(sceSymbolDbPath);
        }
 
        if (!analyzer.analyze())
        {
            std::cerr << "Failed to analyze ELF file\n";
            return 1;
        }
 
        if (!analyzer.generateToml(tomlPath))
        {
            std::cerr << "Failed to generate TOML configuration\n";
            return 1;
        }

        if (!emitMapPath.empty())
        {
            ps2recomp::ElfParser parser(elfPath);
            if (!parser.parse())
            {
                std::cerr << "Failed to parse ELF for map emission\n";
                return 1;
            }

            std::vector<uint32_t> seeds;
            for (const std::string &seedFile : seedFiles)
            {
                const std::vector<uint32_t> fromFile = ps2recomp::MapEmitter::readAddresses(seedFile);
                seeds.insert(seeds.end(), fromFile.begin(), fromFile.end());
            }

            ps2recomp::MapEmitter emitter(parser);
            const std::vector<ps2recomp::MapEntry> entries = emitter.build(seeds);
            if (!ps2recomp::MapEmitter::writeCsv(emitMapPath, entries))
            {
                std::cerr << "Failed to write function map to " << emitMapPath << "\n";
                return 1;
            }
            std::cout << "\nEmitted " << entries.size() << " function entries to "
                      << emitMapPath << " (" << seeds.size() << " seed addresses)\n";
        }

        std::cout << "\nAnalysis complete\n";
        std::cout << "TOML configuration has been written to: " << tomlPath << "\n";
        std::cout << "\nYou can now use this configuration with PS2Recomp:\n";
        std::cout << "  ps2recomp " << tomlPath << "\n";

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
