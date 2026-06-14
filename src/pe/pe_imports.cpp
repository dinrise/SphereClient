#include "pe/pe_imports.hpp"

#include "common/binary_reader.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace sphere::pe {
namespace {

struct RvaSize {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

std::string fixed_name(const bin::ByteBuffer& buffer, std::size_t offset, std::size_t max_size) {
    bin::require_range(buffer, offset, max_size, "fixed PE name");
    std::size_t size = 0;
    while (size < max_size && buffer[offset + size] != 0) {
        ++size;
    }
    return std::string(reinterpret_cast<const char*>(buffer.data() + offset), size);
}

std::string c_string_at(const bin::ByteBuffer& buffer, std::size_t offset) {
    std::size_t cursor = offset;
    return bin::read_c_string(buffer, cursor);
}

std::size_t rva_to_file_offset(const std::vector<Section>& sections, std::uint32_t rva) {
    for (const auto& section : sections) {
        const auto mapped_size = std::max(section.virtual_size, section.raw_size);
        if (rva >= section.virtual_address && rva < section.virtual_address + mapped_size) {
            return section.raw_offset + (rva - section.virtual_address);
        }
    }
    throw std::runtime_error("RVA does not map to a section");
}

std::string ordinal_name(std::uint16_t ordinal) {
    std::ostringstream out;
    out << '#' << ordinal;
    return out.str();
}

} // namespace

PeImage parse_imports(const std::filesystem::path& path) {
    auto buffer = bin::read_file(path);
    if (buffer.size() < 0x40 || bin::u16le(buffer, 0) != 0x5A4D) {
        throw std::runtime_error(path.string() + " is not an MZ executable");
    }

    const auto pe_offset = bin::u32le(buffer, 0x3C);
    bin::require_range(buffer, pe_offset, 4 + 20, "PE header");
    if (bin::u32le(buffer, pe_offset) != 0x00004550) {
        throw std::runtime_error(path.string() + " is not a PE executable");
    }

    const auto coff = pe_offset + 4;
    PeImage image;
    image.path = path;
    image.machine = bin::u16le(buffer, coff);
    const auto section_count = bin::u16le(buffer, coff + 2);
    const auto optional_size = bin::u16le(buffer, coff + 16);
    const auto optional = coff + 20;
    bin::require_range(buffer, optional, optional_size, "PE optional header");

    const auto magic = bin::u16le(buffer, optional);
    image.pe32_plus = magic == 0x20B;
    if (magic != 0x10B && magic != 0x20B) {
        throw std::runtime_error("unsupported PE optional header magic");
    }

    image.entry_point_rva = bin::u32le(buffer, optional + 16);
    if (image.pe32_plus) {
        image.image_base64 = static_cast<std::uint64_t>(bin::u32le(buffer, optional + 24))
            | (static_cast<std::uint64_t>(bin::u32le(buffer, optional + 28)) << 32);
    } else {
        image.image_base32 = bin::u32le(buffer, optional + 28);
    }

    const auto number_of_rva_and_sizes = bin::u32le(buffer, optional + (image.pe32_plus ? 108 : 92));
    const auto data_directory = optional + (image.pe32_plus ? 112 : 96);
    if (number_of_rva_and_sizes < 2) {
        return image;
    }
    const RvaSize import_directory{
        bin::u32le(buffer, data_directory + 8),
        bin::u32le(buffer, data_directory + 12),
    };

    const auto section_table = optional + optional_size;
    bin::require_range(buffer, section_table, static_cast<std::size_t>(section_count) * 40, "PE section table");
    image.sections.reserve(section_count);
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const auto off = section_table + static_cast<std::uint32_t>(i) * 40;
        Section section;
        section.name = fixed_name(buffer, off, 8);
        section.virtual_size = bin::u32le(buffer, off + 8);
        section.virtual_address = bin::u32le(buffer, off + 12);
        section.raw_size = bin::u32le(buffer, off + 16);
        section.raw_offset = bin::u32le(buffer, off + 20);
        image.sections.push_back(std::move(section));
    }

    if (import_directory.rva == 0 || import_directory.size == 0) {
        return image;
    }

    std::size_t descriptor_offset = rva_to_file_offset(image.sections, import_directory.rva);
    while (true) {
        bin::require_range(buffer, descriptor_offset, 20, "import descriptor");
        const auto original_first_thunk = bin::u32le(buffer, descriptor_offset);
        const auto name_rva = bin::u32le(buffer, descriptor_offset + 12);
        const auto first_thunk = bin::u32le(buffer, descriptor_offset + 16);
        if (original_first_thunk == 0 && name_rva == 0 && first_thunk == 0) {
            break;
        }

        ImportLibrary library;
        library.name = c_string_at(buffer, rva_to_file_offset(image.sections, name_rva));

        const auto thunk_rva = original_first_thunk != 0 ? original_first_thunk : first_thunk;
        std::size_t thunk_offset = rva_to_file_offset(image.sections, thunk_rva);
        std::uint32_t thunk_index = 0;
        const std::uint32_t thunk_stride = image.pe32_plus ? 8U : 4U;
        while (true) {
            const auto thunk_value = image.pe32_plus ? bin::u64le(buffer, thunk_offset) : bin::u32le(buffer, thunk_offset);
            if (thunk_value == 0) {
                break;
            }

            ImportSymbol symbol;
            symbol.thunk_rva = first_thunk + thunk_index * thunk_stride;
            const auto ordinal_mask = image.pe32_plus ? 0x8000000000000000ULL : 0x80000000ULL;
            if ((thunk_value & ordinal_mask) != 0) {
                const auto ordinal = static_cast<std::uint16_t>(thunk_value & 0xFFFFU);
                symbol.ordinal = ordinal;
                symbol.name = ordinal_name(ordinal);
            } else {
                const auto import_by_name = rva_to_file_offset(image.sections, static_cast<std::uint32_t>(thunk_value));
                symbol.name = c_string_at(buffer, import_by_name + 2);
            }

            library.symbols.push_back(std::move(symbol));
            thunk_offset += thunk_stride;
            ++thunk_index;
        }

        image.imports.push_back(std::move(library));
        descriptor_offset += 20;
    }

    return image;
}

} // namespace sphere::pe
