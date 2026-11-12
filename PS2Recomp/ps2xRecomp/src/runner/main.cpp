#include "ps2recomp/ps2_recompiler.h"
#include <iostream>
#include <string>

using namespace ps2recomp;

void printUsage()
{
    std::cout << "PS2Recomp - A static recompiler for PlayStation 2 ELF files\n";
    std::cout << "Usage: ps2recomp <config.toml>\n";
    std::cout << "       ps2recomp --check-map <config.toml>\n";
    std::cout << "  config.toml: Configuration file for the recompiler\n";
    std::cout << "  --check-map: Only validate the function map named by ghidra_output\n";
}

int main(int argc, char *argv[])
{
    bool checkMapOnly = false;
    std::string configPath;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--check-map")
        {
            checkMapOnly = true;
        }
        else if (configPath.empty())
        {
            configPath = argument;
        }
    }

    if (configPath.empty())
    {
        printUsage();
        return 1;
    }

    try
    {
        PS2Recompiler recompiler(configPath);

        if (checkMapOnly)
        {
            return recompiler.checkFunctionMap() ? 0 : 1;
        }

        if (!recompiler.initialize())
        {
            std::cerr << "Failed to initialize recompiler\n";
            recompiler.printReport();
            return 1;
        }

        if (!recompiler.recompile())
        {
            std::cerr << "Recompilation failed\n";
            recompiler.printReport();
            return 1;
        }

        recompiler.generateOutput();
        recompiler.printReport();

        std::cout << "Recompilation completed successfully\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}