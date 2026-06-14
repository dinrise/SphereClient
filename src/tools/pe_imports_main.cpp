#include "pe/pe_imports.hpp"

#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string hex32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

void usage() {
    std::cout << "usage: pe_imports <path-to-exe>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage();
        return 1;
    }

    try {
        const auto image = sphere::pe::parse_imports(argv[1]);
        std::size_t import_count = 0;
        for (const auto& library : image.imports) {
            import_count += library.symbols.size();
        }

        std::cout << image.path.string() << "\n";
        std::cout << "machine=" << hex32(image.machine)
                  << " format=" << (image.pe32_plus ? "PE32+" : "PE32")
                  << " entry_rva=" << hex32(image.entry_point_rva)
                  << " libraries=" << image.imports.size()
                  << " imports=" << import_count << "\n";

        std::cout << "sections:\n";
        for (const auto& section : image.sections) {
            std::cout << "  " << section.name
                      << " va=" << hex32(section.virtual_address)
                      << " vs=" << hex32(section.virtual_size)
                      << " raw=" << hex32(section.raw_offset)
                      << " raw_size=" << hex32(section.raw_size) << "\n";
        }

        std::cout << "imports:\n";
        for (const auto& library : image.imports) {
            std::cout << "  " << library.name << " (" << library.symbols.size() << ")\n";
            for (const auto& symbol : library.symbols) {
                std::cout << "    " << symbol.name
                          << " thunk=" << hex32(symbol.thunk_rva) << "\n";
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
