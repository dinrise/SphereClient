#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sphere::model {

struct MdlSection {
    std::string name;
    std::size_t offset = 0;
    std::size_t count = 0;
    std::size_t stride = 0;
    std::size_t size = 0;
};

struct MdlInfo {
    std::filesystem::path path;
    std::size_t file_size = 0;
    std::uint16_t vertex_count = 0;
    std::uint16_t index_count = 0;
    std::uint16_t triangle_group_count = 0;
    std::uint8_t material_count = 0;
    std::uint16_t material_names_size = 0;
    std::uint8_t strip_group_count = 0;
    std::uint8_t small_block_size = 0;
    std::uint8_t unknown_flag_0f = 0;
    std::uint16_t skin_record_count = 0;
    std::uint16_t skin_index_count = 0;
    std::uint8_t skin_weight_count = 0;
    std::uint8_t animation_flag = 0;
    std::uint16_t animation_table_count = 0;
    std::int32_t extra_mode = 0;
    std::int32_t extra_record_count = 0;
    std::int32_t extra_block_count = 0;
    std::vector<std::string> materials;
    std::vector<MdlSection> sections;
    std::size_t computed_size = 0;
};

struct MdlVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct MdlTriangle {
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    std::uint16_t c = 0;
    std::uint16_t flags = 0;
    std::uint16_t reserved = 0;
};

struct MdlSurface {
    std::uint8_t object_index = 0;
    std::uint8_t texture_index = 0;
    std::int16_t first_triangle_index = 0;
    std::int16_t triangle_count = 0;
    std::int16_t first_vertex_index = 0;
    std::int16_t vertex_count = 0;
};

struct MdlObject {
    std::string name;
    std::uint8_t bone_type = 0;
    std::uint8_t connected_bone_count = 0;
    std::uint8_t object_index_offset = 0;
    std::uint8_t is_animated = 0;
    std::int16_t key_index = 0;
    std::uint8_t parent_index = 0;
};

struct MdlTransformKey {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
};

struct MdlBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float min_z = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float max_z = 0.0f;
};

struct MdlMesh {
    MdlInfo info;
    std::vector<MdlVertex> vertices;
    std::vector<MdlTriangle> triangles;
    std::vector<MdlSurface> surfaces;
    std::vector<MdlObject> objects;
    std::vector<std::uint8_t> object_indices;
    std::vector<MdlTransformKey> transform_keys;
    std::vector<std::uint16_t> actions;
    MdlBounds bounds;
};

MdlInfo load_mdl_info(const std::filesystem::path& path);
MdlMesh load_mdl_mesh(const std::filesystem::path& path);

} // namespace sphere::model
