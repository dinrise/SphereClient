#include "mbc/mbc.hpp"

#include "common/binary_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace sphere::mbc {
namespace {

std::string read_magic(const bin::ByteBuffer& buffer) {
    bin::require_range(buffer, 0, 16, "MBC magic");
    auto end = std::find(buffer.begin(), buffer.begin() + 16, 0);
    return std::string(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(end - buffer.begin()));
}

void validate_magic(const bin::ByteBuffer& buffer, const std::filesystem::path& path) {
    static constexpr std::array<std::uint8_t, 16> expected = {
        'M', 'B', 'L', ' ', 's', 'c', 'r', 'i',
        'p', 't', ' ', 'v', '4', '.', '0', '\0',
    };
    bin::require_range(buffer, 0, expected.size(), "MBC magic");
    if (!std::equal(expected.begin(), expected.end(), buffer.begin())) {
        throw std::runtime_error(path.string() + " has bad MBC magic");
    }
}

} // namespace

std::int8_t MbcProgram::state() const {
    return static_cast<std::int8_t>(state_raw < 0x80 ? state_raw : state_raw - 0x100);
}

std::uint32_t MbcProgram::file_start() const {
    return kCodeFileOffset + start;
}

std::uint32_t MbcProgram::file_end() const {
    return kCodeFileOffset + end;
}

std::int32_t MbcFunction::program_index() const {
    std::int64_t value = program_index_raw;
    if (program_index_raw >= 0x80000000U) {
        value -= 0x100000000LL;
    }
    return static_cast<std::int32_t>(value);
}

bool MbcFunction::is_import() const {
    return program_index() < 0;
}

std::uint32_t MbcFunction::file_offset() const {
    return kCodeFileOffset + code_offset;
}

const MbcProgram* MbcScript::program_by_name(const std::string& name) const {
    const auto it = std::find_if(programs.begin(), programs.end(), [&](const MbcProgram& program) {
        return program.name == name;
    });
    return it == programs.end() ? nullptr : &*it;
}

const MbcProgram* MbcScript::program_for_offset(std::uint32_t code_offset) const {
    const auto it = std::find_if(programs.begin(), programs.end(), [&](const MbcProgram& program) {
        return program.start <= code_offset && code_offset <= program.end;
    });
    return it == programs.end() ? nullptr : &*it;
}

MbcScript load_script(const std::filesystem::path& path) {
    auto buffer = bin::read_file(path);
    if (buffer.size() < kCodeFileOffset) {
        throw std::runtime_error(path.string() + " is too small to be an MBC file");
    }

    validate_magic(buffer, path);

    MbcScript script;
    script.path = path;
    script.header.magic = read_magic(buffer);
    script.header.checksum_or_tag = bin::u32le(buffer, 0x10);
    script.header.module_tag = bin::u32le(buffer, 0x14);
    script.header.code_size = bin::u32le(buffer, 0x18);
    script.header.data_size = bin::u32le(buffer, 0x1C);

    const std::size_t code_start = kCodeFileOffset;
    const std::size_t code_end = code_start + script.header.code_size;
    const std::size_t data_end = code_end + script.header.data_size;
    if (data_end > buffer.size()) {
        throw std::runtime_error(path.string() + " is truncated before the end of code/data sections");
    }

    script.code.assign(buffer.begin() + static_cast<std::ptrdiff_t>(code_start), buffer.begin() + static_cast<std::ptrdiff_t>(code_end));
    script.data.assign(buffer.begin() + static_cast<std::ptrdiff_t>(code_end), buffer.begin() + static_cast<std::ptrdiff_t>(data_end));

    std::size_t offset = data_end;
    const auto program_count = bin::u32le(buffer, offset);
    offset += 4;
    script.programs.reserve(program_count);
    for (std::uint32_t i = 0; i < program_count; ++i) {
        MbcProgram program;
        program.index = i;
        program.name = bin::read_c_string(buffer, offset);
        bin::require_range(buffer, offset, 14, "MBC program record");
        program.start = bin::u32le(buffer, offset);
        program.end = bin::u32le(buffer, offset + 4);
        program.state_raw = bin::u8(buffer, offset + 8);
        program.queue_id = bin::u8(buffer, offset + 9);
        program.unknown_48 = bin::u32le(buffer, offset + 10);
        offset += 14;
        script.programs.push_back(std::move(program));
    }

    const auto function_count = bin::u32le(buffer, offset);
    offset += 4;
    script.functions.reserve(function_count);
    for (std::uint32_t i = 0; i < function_count; ++i) {
        MbcFunction function;
        function.index = i;
        function.name = bin::read_c_string(buffer, offset);
        bin::require_range(buffer, offset, 12, "MBC function record");
        function.code_offset = bin::u32le(buffer, offset);
        function.program_index_raw = bin::u32le(buffer, offset + 4);
        function.flags_or_module = bin::u32le(buffer, offset + 8);
        offset += 12;
        script.functions.push_back(std::move(function));
    }

    if (offset < buffer.size()) {
        script.metadata.assign(buffer.begin() + static_cast<std::ptrdiff_t>(offset), buffer.end());
    }

    return script;
}

std::vector<AdbArrayGuard> load_adb(const std::filesystem::path& path) {
    auto buffer = bin::read_file(path);
    if (buffer.size() % 8 != 0) {
        throw std::runtime_error(path.string() + " size must be divisible by 8");
    }

    std::vector<AdbArrayGuard> guards;
    guards.reserve(buffer.size() / 8);
    for (std::size_t i = 0; i < buffer.size() / 8; ++i) {
        const auto offset = i * 8;
        guards.push_back(AdbArrayGuard{
            static_cast<std::uint32_t>(i),
            bin::u32le(buffer, offset),
            bin::u32le(buffer, offset + 4),
        });
    }
    return guards;
}

} // namespace sphere::mbc
