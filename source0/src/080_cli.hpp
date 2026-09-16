#pragma once

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace pge_treemap {

namespace fs = std::filesystem;

struct CliOptions {
    fs::path targetDir = fs::current_path();
};

inline std::optional<CliOptions> ParseCli(int argc, char* argv[])
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [DIRECTORY] [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  -d, --dir <PATH>     Directory to visualize\n"
                      << "  -h, --help           Display this help and exit\n\n"
                      << "If no directory is provided, the current working directory is used.\n"
                      << "On Linux with inotify, changes inside the directory automatically update the treemap.\n";
            return std::nullopt;
        }

        if ((arg == "-d" || arg == "--dir" || arg == "-p" || arg == "--path") && i + 1 < argc)
        {
            options.targetDir = argv[++i];
        }
        else if (!arg.starts_with('-'))
        {
            options.targetDir = arg;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n"
                      << "Run '" << argv[0] << " --help' for usage.\n";
            return std::nullopt;
        }
    }

    std::error_code ec;
    options.targetDir = fs::absolute(options.targetDir, ec);
    if (ec || !fs::exists(options.targetDir, ec))
    {
        std::cerr << "Error: Path does not exist: " << options.targetDir.string() << "\n";
        return std::nullopt;
    }
    if (!fs::is_directory(options.targetDir, ec))
    {
        std::cerr << "Error: Path is not a directory: " << options.targetDir.string() << "\n";
        return std::nullopt;
    }

    return options;
}

} // namespace pge_treemap
