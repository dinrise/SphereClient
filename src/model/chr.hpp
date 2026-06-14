#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sphere::model {

struct ChrChunk {
    std::int32_t type = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct ChrInfo {
    std::filesystem::path path;
    std::size_t file_size = 0;
    std::int32_t version_or_flags = 0;
    std::int32_t chunk_count = 0;
    std::vector<ChrChunk> chunks;
    std::vector<std::string> bone_names;
};

struct ChrVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint8_t bone0 = 0;
    std::uint8_t bone1 = 0;
    float blend = 1.0f;
};

struct ChrBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float min_z = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float max_z = 0.0f;
};

struct ChrMesh {
    ChrInfo info;
    std::vector<ChrVertex> vertices;
    std::vector<std::uint16_t> indices;
    ChrBounds bounds;
};

ChrInfo load_chr_info(const std::filesystem::path& path);
ChrMesh load_chr_mesh(const std::filesystem::path& path);

} // namespace sphere::model
