#include "mbc/mbc.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path input;
    std::filesystem::path adb;
    std::size_t limit = 16;
    bool list_functions = false;
};

void usage() {
    std::cout
        << "usage: mbc_dump <file-or-dir> [--adb <file.adb>] [--limit N] [--functions]\n";
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--adb" && i + 1 < argc) {
            options.adb = argv[++i];
        } else if (arg == "--limit" && i + 1 < argc) {
            options.limit = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--functions") {
            options.list_functions = true;
        } else if (options.input.empty()) {
            options.input = arg;
        } else {
            throw std::runtime_error("unexpected argument: " + arg);
        }
    }
    if (options.input.empty()) {
        throw std::runtime_error("missing input path");
    }
    return options;
}

std::string hex32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

void dump_script(const std::filesystem::path& path, const Options& options) {
    const auto script = sphere::mbc::load_script(path);
    std::cout << "\n" << path.string() << "\n";
    std::cout << "  magic=" << script.header.magic
              << " checksum=" << hex32(script.header.checksum_or_tag)
              << " module_tag=" << script.header.module_tag << "\n";
    std::cout << "  code=" << script.code.size()
              << " data=" << script.data.size()
              << " programs=" << script.programs.size()
              << " functions=" << script.functions.size()
              << " metadata=" << script.metadata.size() << "\n";

    const auto program_limit = std::min(options.limit, script.programs.size());
    std::cout << "  programs:\n";
    for (std::size_t i = 0; i < program_limit; ++i) {
        const auto& program = script.programs[i];
        std::cout << "    [" << program.index << "] " << program.name
                  << " start=" << hex32(program.start)
                  << " end=" << hex32(program.end)
                  << " state=" << static_cast<int>(program.state())
                  << " queue=" << static_cast<int>(program.queue_id) << "\n";
    }

    const auto function_limit = options.list_functions ? script.functions.size() : std::min(options.limit, script.functions.size());
    std::cout << "  functions:\n";
    for (std::size_t i = 0; i < function_limit; ++i) {
        const auto& function = script.functions[i];
        std::cout << "    [" << function.index << "] " << function.name
                  << " code=" << hex32(function.code_offset)
                  << " file=" << hex32(function.file_offset())
                  << " program=" << function.program_index();
        if (function.is_import()) {
            std::cout << " import";
        }
        std::cout << "\n";
    }

    auto adb_path = options.adb;
    if (adb_path.empty()) {
        adb_path = path;
        adb_path.replace_extension(".adb");
    }
    if (std::filesystem::exists(adb_path)) {
        const auto guards = sphere::mbc::load_adb(adb_path);
        std::cout << "  adb_guards=" << guards.size() << "\n";
        const auto guard_limit = std::min(options.limit, guards.size());
        for (std::size_t i = 0; i < guard_limit; ++i) {
            const auto& guard = guards[i];
            std::cout << "    [" << guard.index << "] begin=" << hex32(guard.begin_guard_offset)
                      << " end=" << hex32(guard.end_guard_offset) << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        if (std::filesystem::is_directory(options.input)) {
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::directory_iterator(options.input)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mbc") {
                    paths.push_back(entry.path());
                }
            }
            std::sort(paths.begin(), paths.end());
            for (const auto& path : paths) {
                dump_script(path, options);
            }
        } else {
            dump_script(options.input, options);
        }
        return 0;
    } catch (const std::exception& ex) {
        usage();
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
