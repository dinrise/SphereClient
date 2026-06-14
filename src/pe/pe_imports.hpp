#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sphere::pe {

struct ImportSymbol {
    std::optional<std::uint16_t> ordinal;
    std::string name;
    std::uint32_t thunk_rva = 0;
};

struct ImportLibrary {
    std::string name;
    std::vector<ImportSymbol> symbols;
};

struct Section {
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
};

struct PeImage {
    std::filesystem::path path;
    bool pe32_plus = false;
    std::uint16_t machine = 0;
    std::uint32_t image_base32 = 0;
    std::uint64_t image_base64 = 0;
    std::uint32_t entry_point_rva = 0;
    std::vector<Section> sections;
    std::vector<ImportLibrary> imports;
};

PeImage parse_imports(const std::filesystem::path& path);

} // namespace sphere::pe
