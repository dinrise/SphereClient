#include "model/mdl.hpp"

#include "common/binary_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace sphere::model {
namespace {

void add_section(MdlInfo& info, std::string name, std::size_t& offset, std::size_t count, std::size_t stride) {
    const std::size_t size = count * stride;
    info.sections.push_back(MdlSection{std::move(name), offset, count, stride, size});
    offset += size;
}

void read_materials(MdlInfo& info, const bin::ByteBuffer& data) {
    constexpr std::size_t names_offset = 0x102;
    bin::require_range(data, names_offset, info.material_names_size, "MDL material names");
    std::size_t cursor = names_offset;
    const std::size_t names_end = names_offset + info.material_names_size;
    for (std::uint8_t i = 0; i < info.material_count; ++i) {
        if (cursor >= names_end) {
            throw std::runtime_error("truncated MDL material name table");
        }
        const auto length = data[cursor++];
        if (cursor + length > names_end) {
            throw std::runtime_error("truncated MDL material name");
        }
        info.materials.emplace_back(reinterpret_cast<const char*>(data.data() + cursor), length);
        cursor += length;
    }
}

const MdlSection& section_by_name(const MdlInfo& info, const std::string& name) {
    const auto it = std::find_if(info.sections.begin(), info.sections.end(), [&](const MdlSection& section) {
        return section.name == name;
    });
    if (it == info.sections.end()) {
        throw std::runtime_error("missing MDL section: " + name);
    }
    return *it;
}

const MdlSection* find_section_by_name(const MdlInfo& info, const std::string& name) {
    const auto it = std::find_if(info.sections.begin(), info.sections.end(), [&](const MdlSection& section) {
        return section.name == name;
    });
    if (it == info.sections.end()) {
        return nullptr;
    }
    return &*it;
}

std::string read_fixed_string(const bin::ByteBuffer& data, std::size_t offset, std::size_t size) {
    bin::require_range(data, offset, size, "fixed string");
    const auto* begin = reinterpret_cast<const char*>(data.data() + offset);
    std::size_t length = 0;
    while (length < size && begin[length] != '\0') {
        ++length;
    }
    return std::string(begin, length);
}

void read_vertices(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto& section = section_by_name(mesh.info, "vertices_0x20");
    bin::require_range(data, section.offset, section.size, "MDL vertices");
    mesh.vertices.reserve(section.count);

    for (std::size_t i = 0; i < section.count; ++i) {
        const std::size_t offset = section.offset + i * section.stride;
        MdlVertex vertex;
        vertex.x = bin::f32le(data, offset + 0x00);
        vertex.y = bin::f32le(data, offset + 0x04);
        vertex.z = bin::f32le(data, offset + 0x08);
        vertex.nx = bin::f32le(data, offset + 0x0c);
        vertex.ny = bin::f32le(data, offset + 0x10);
        vertex.nz = bin::f32le(data, offset + 0x14);
        vertex.u = bin::f32le(data, offset + 0x18);
        vertex.v = bin::f32le(data, offset + 0x1c);
        mesh.vertices.push_back(vertex);
    }
}

void read_triangles(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto& section = section_by_name(mesh.info, "indices_0x0a");
    bin::require_range(data, section.offset, section.size, "MDL triangle records");
    mesh.triangles.reserve(section.count);

    for (std::size_t i = 0; i < section.count; ++i) {
        const std::size_t offset = section.offset + i * section.stride;
        MdlTriangle triangle;
        triangle.a = bin::u16le(data, offset + 0x00);
        triangle.b = bin::u16le(data, offset + 0x02);
        triangle.c = bin::u16le(data, offset + 0x04);
        triangle.flags = bin::u16le(data, offset + 0x06);
        triangle.reserved = bin::u16le(data, offset + 0x08);
        mesh.triangles.push_back(triangle);
    }
}

void read_surfaces(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto& section = section_by_name(mesh.info, "triangle_groups_0x0f");
    bin::require_range(data, section.offset, section.size, "MDL surfaces");
    mesh.surfaces.reserve(section.count);

    for (std::size_t i = 0; i < section.count; ++i) {
        const std::size_t offset = section.offset + i * section.stride;
        MdlSurface surface;
        surface.object_index = bin::u8(data, offset + 0x00);
        surface.texture_index = bin::u8(data, offset + 0x01);
        surface.first_triangle_index = bin::i16le(data, offset + 0x02);
        surface.triangle_count = bin::i16le(data, offset + 0x04);
        surface.first_vertex_index = bin::i16le(data, offset + 0x06);
        surface.vertex_count = bin::i16le(data, offset + 0x08);
        mesh.surfaces.push_back(surface);
    }
}

void read_objects(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto* section = find_section_by_name(mesh.info, "strip_groups_0x27");
    if (!section) {
        return;
    }
    bin::require_range(data, section->offset, section->size, "MDL objects");
    mesh.objects.reserve(section->count);

    for (std::size_t i = 0; i < section->count; ++i) {
        const std::size_t offset = section->offset + i * section->stride;
        MdlObject object;
        object.name = read_fixed_string(data, offset, 32);
        object.bone_type = bin::u8(data, offset + 0x20);
        object.connected_bone_count = bin::u8(data, offset + 0x21);
        object.object_index_offset = bin::u8(data, offset + 0x22);
        object.is_animated = bin::u8(data, offset + 0x23);
        object.key_index = bin::i16le(data, offset + 0x24);
        object.parent_index = bin::u8(data, offset + 0x26);
        mesh.objects.push_back(std::move(object));
    }
}

void read_object_indices(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto* section = find_section_by_name(mesh.info, "small_block");
    if (!section) {
        return;
    }
    bin::require_range(data, section->offset, section->size, "MDL object indices");
    mesh.object_indices.assign(data.begin() + static_cast<std::ptrdiff_t>(section->offset),
                               data.begin() + static_cast<std::ptrdiff_t>(section->offset + section->size));
}

void read_transform_keys(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto* section = find_section_by_name(mesh.info, "skin_records_0x1c");
    if (!section) {
        return;
    }
    bin::require_range(data, section->offset, section->size, "MDL transform keys");
    mesh.transform_keys.reserve(section->count);

    for (std::size_t i = 0; i < section->count; ++i) {
        const std::size_t offset = section->offset + i * section->stride;
        MdlTransformKey key;
        key.x = bin::f32le(data, offset + 0x00);
        key.y = bin::f32le(data, offset + 0x04);
        key.z = bin::f32le(data, offset + 0x08);
        // Quaternion order is (w,x,y,z): the .mdl skeleton path FUN_00454ff0 feeds these to
        // FUN_0044bb80 with param[0]=w (verified live via the debugger — the quat at the call
        // site reads w first, and the resulting bone matrices match the running original).
        key.qw = bin::f32le(data, offset + 0x0c);
        key.qx = bin::f32le(data, offset + 0x10);
        key.qy = bin::f32le(data, offset + 0x14);
        key.qz = bin::f32le(data, offset + 0x18);
        mesh.transform_keys.push_back(key);
    }
}

void read_skin_indices(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto* section = find_section_by_name(mesh.info, "skin_indices_0x03");
    if (!section) {
        return;
    }
    bin::require_range(data, section->offset, section->size, "MDL skin indices");
    mesh.skin_indices.reserve(section->count);
    for (std::size_t i = 0; i < section->count; ++i) {
        const std::size_t offset = section->offset + i * section->stride;
        MdlSkinIndex entry;
        entry.record = bin::u16le(data, offset + 0x00);
        entry.blend = bin::u8(data, offset + 0x02);
        mesh.skin_indices.push_back(entry);
    }
}

void read_actions(MdlMesh& mesh, const bin::ByteBuffer& data) {
    const auto* section = find_section_by_name(mesh.info, "skin_weights_0x02");
    if (!section) {
        return;
    }
    bin::require_range(data, section->offset, section->size, "MDL actions");
    mesh.actions.reserve(section->count);

    for (std::size_t i = 0; i < section->count; ++i) {
        mesh.actions.push_back(bin::u16le(data, section->offset + i * section->stride));
    }
}

MdlBounds compute_bounds(const std::vector<MdlVertex>& vertices) {
    if (vertices.empty()) {
        return {};
    }

    MdlBounds bounds;
    bounds.min_x = bounds.max_x = vertices.front().x;
    bounds.min_y = bounds.max_y = vertices.front().y;
    bounds.min_z = bounds.max_z = vertices.front().z;
    for (const auto& vertex : vertices) {
        bounds.min_x = std::min(bounds.min_x, vertex.x);
        bounds.min_y = std::min(bounds.min_y, vertex.y);
        bounds.min_z = std::min(bounds.min_z, vertex.z);
        bounds.max_x = std::max(bounds.max_x, vertex.x);
        bounds.max_y = std::max(bounds.max_y, vertex.y);
        bounds.max_z = std::max(bounds.max_z, vertex.z);
    }
    return bounds;
}

void validate_counts(const MdlInfo& info) {
    if (info.extra_mode == 2 && (info.extra_record_count < 0 || info.extra_block_count < 0)) {
        throw std::runtime_error("negative MDL extra section count");
    }
}

} // namespace

MdlInfo load_mdl_info(const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    if (data.size() < 0x102 || data[0] != 'M' || data[1] != 'D' || data[2] != 'L' || data[3] != '!') {
        throw std::runtime_error("bad MDL file: " + path.string());
    }

    MdlInfo info;
    info.path = path;
    info.file_size = data.size();
    info.vertex_count = bin::u16le(data, 0x04);
    info.index_count = bin::u16le(data, 0x06);
    info.triangle_group_count = bin::u16le(data, 0x08);
    info.material_count = bin::u8(data, 0x0a);
    info.material_names_size = bin::u16le(data, 0x0b);
    info.strip_group_count = bin::u8(data, 0x0d);
    info.small_block_size = bin::u8(data, 0x0e);
    info.unknown_flag_0f = bin::u8(data, 0x0f);
    info.skin_record_count = bin::u16le(data, 0x10);
    info.skin_index_count = bin::u16le(data, 0x12);
    info.skin_weight_count = bin::u8(data, 0x14);
    info.animation_flag = bin::u8(data, 0x17);
    info.animation_table_count = bin::u16le(data, 0x18);
    info.extra_mode = bin::i32le(data, 0x1a);
    info.extra_record_count = bin::i32le(data, 0xfa);
    info.extra_block_count = bin::i32le(data, 0xfe);
    validate_counts(info);

    read_materials(info, data);

    info.sections.push_back(MdlSection{"material_names", 0x102, info.material_count, 0, info.material_names_size});
    std::size_t offset = 0x102 + info.material_names_size;
    add_section(info, "vertices_0x20", offset, info.vertex_count, 0x20);
    add_section(info, "indices_0x0a", offset, info.index_count, 0x0a);
    add_section(info, "triangle_groups_0x0f", offset, info.triangle_group_count, 0x0f);
    add_section(info, "strip_groups_0x27", offset, info.strip_group_count, 0x27);
    add_section(info, "small_block", offset, info.small_block_size, 1);
    if (info.skin_weight_count != 0) {
        add_section(info, "skin_records_0x1c", offset, info.skin_record_count, 0x1c);
        add_section(info, "skin_indices_0x03", offset, info.skin_index_count, 0x03);
        add_section(info, "skin_weights_0x02", offset, info.skin_weight_count, 0x02);
    } else {
        add_section(info, "strip_fallback_0x18", offset, info.strip_group_count, 0x18);
    }
    if (info.animation_flag == 1) {
        add_section(info, "animation_table_0x04", offset, info.animation_table_count, 0x04);
        add_section(info, "animation_indices_0x06", offset, info.index_count, 0x06);
    }
    if (info.extra_mode == 2) {
        add_section(info, "extra_records_0x0c", offset, static_cast<std::size_t>(info.extra_record_count), 0x0c);
        add_section(info, "extra_blocks_0x50", offset, static_cast<std::size_t>(info.extra_block_count), 0x50);
    }
    info.computed_size = offset;
    if (info.computed_size != info.file_size) {
        throw std::runtime_error("MDL section size mismatch: " + path.string());
    }
    return info;
}

MdlMesh load_mdl_mesh(const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    MdlMesh mesh;
    mesh.info = load_mdl_info(path);
    read_vertices(mesh, data);
    read_triangles(mesh, data);
    read_surfaces(mesh, data);
    read_objects(mesh, data);
    read_object_indices(mesh, data);
    read_transform_keys(mesh, data);
    read_skin_indices(mesh, data);
    read_actions(mesh, data);
    mesh.bounds = compute_bounds(mesh.vertices);
    return mesh;
}

} // namespace sphere::model
