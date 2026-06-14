#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sphere::bin {

using ByteBuffer = std::vector<std::uint8_t>;

inline ByteBuffer read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("failed to determine file size: " + path.string());
    }
    file.seekg(0, std::ios::beg);

    ByteBuffer data(static_cast<std::size_t>(size));
    if (!data.empty()) {
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) {
            throw std::runtime_error("failed to read file: " + path.string());
        }
    }
    return data;
}

inline void require_range(const ByteBuffer& data, std::size_t offset, std::size_t size, std::string_view what) {
    if (offset > data.size() || size > data.size() - offset) {
        throw std::runtime_error(std::string("truncated data while reading ") + std::string(what));
    }
}

inline std::uint8_t u8(const ByteBuffer& data, std::size_t offset) {
    require_range(data, offset, 1, "u8");
    return data[offset];
}

inline std::uint16_t u16le(const ByteBuffer& data, std::size_t offset) {
    require_range(data, offset, 2, "u16");
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

inline std::int16_t i16le(const ByteBuffer& data, std::size_t offset) {
    return static_cast<std::int16_t>(u16le(data, offset));
}

inline std::uint32_t u32le(const ByteBuffer& data, std::size_t offset) {
    require_range(data, offset, 4, "u32");
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

inline std::int32_t i32le(const ByteBuffer& data, std::size_t offset) {
    return static_cast<std::int32_t>(u32le(data, offset));
}

inline float f32le(const ByteBuffer& data, std::size_t offset) {
    const std::uint32_t value = u32le(data, offset);
    float out = 0.0f;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

inline std::uint64_t u64le(const ByteBuffer& data, std::size_t offset) {
    require_range(data, offset, 8, "u64");
    return static_cast<std::uint64_t>(u32le(data, offset))
        | (static_cast<std::uint64_t>(u32le(data, offset + 4)) << 32);
}

inline std::string read_c_string(const ByteBuffer& data, std::size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("truncated data while reading cstring");
    }
    const auto start = offset;
    while (offset < data.size() && data[offset] != 0) {
        ++offset;
    }
    if (offset >= data.size()) {
        throw std::runtime_error("unterminated cstring");
    }
    std::string out(reinterpret_cast<const char*>(data.data() + start), offset - start);
    ++offset;
    return out;
}

} // namespace sphere::bin
