#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sphere::mbc {

inline constexpr char kMagic[] = "MBL script v4.0";
inline constexpr std::uint32_t kCodeFileOffset = 0x20;

struct MbcHeader {
    std::string magic;
    std::uint32_t checksum_or_tag = 0;
    std::uint32_t module_tag = 0;
    std::uint32_t code_size = 0;
    std::uint32_t data_size = 0;
};

struct MbcProgram {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint8_t state_raw = 0;
    std::uint8_t queue_id = 0;
    std::uint32_t unknown_48 = 0;

    std::int8_t state() const;
    std::uint32_t file_start() const;
    std::uint32_t file_end() const;
};

struct MbcFunction {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t code_offset = 0;
    std::uint32_t program_index_raw = 0;
    std::uint32_t flags_or_module = 0;

    std::int32_t program_index() const;
    bool is_import() const;
    std::uint32_t file_offset() const;
};

struct AdbArrayGuard {
    std::uint32_t index = 0;
    std::uint32_t begin_guard_offset = 0;
    std::uint32_t end_guard_offset = 0;
};

struct MbcScript {
    std::filesystem::path path;
    MbcHeader header;
    std::vector<std::uint8_t> code;
    std::vector<std::uint8_t> data;
    std::vector<MbcProgram> programs;
    std::vector<MbcFunction> functions;
    std::vector<std::uint8_t> metadata;

    const MbcProgram* program_by_name(const std::string& name) const;
    const MbcProgram* program_for_offset(std::uint32_t code_offset) const;
};

MbcScript load_script(const std::filesystem::path& path);
std::vector<AdbArrayGuard> load_adb(const std::filesystem::path& path);

} // namespace sphere::mbc
