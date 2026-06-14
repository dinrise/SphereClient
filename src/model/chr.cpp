#include "model/chr.hpp"

#include "common/binary_reader.hpp"

#include <algorithm>
#include <stdexcept>

namespace sphere::model {
namespace {

std::string read_length_prefixed_name(const bin::ByteBuffer& data, std::size_t& cursor, std::size_t end) {
    bin::require_range(data, cursor, 1, "CHR bone name length");
    const std::size_t length = bin::u8(data, cursor);
    ++cursor;
    if (cursor > end || length > end - cursor) {
        throw std::runtime_error("truncated CHR bone name");
    }
    std::string name(reinterpret_cast<const char*>(data.data() + cursor), length);
    cursor += length;
    while (!name.empty() && name.back() == '\0') {
        name.pop_back();
    }
    return name;
}

void read_bone_names(ChrInfo& info, const bin::ByteBuffer& data, const ChrChunk& chunk) {
    if (chunk.size < 4) {
        throw std::runtime_error("truncated CHR bone-name chunk");
    }

    const std::size_t end = chunk.offset + chunk.size;
    std::size_t cursor = chunk.offset;
    const std::int32_t count = bin::i32le(data, cursor);
    cursor += 4;
    if (count < 0) {
        throw std::runtime_error("negative CHR bone-name count");
    }

    info.bone_names.clear();
    info.bone_names.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        info.bone_names.push_back(read_length_prefixed_name(data, cursor, end));
    }
}

const ChrChunk* find_chunk(const ChrInfo& info, std::int32_t type) {
    const auto it = std::find_if(info.chunks.begin(), info.chunks.end(), [&](const ChrChunk& chunk) {
        return chunk.type == type;
    });
    return it == info.chunks.end() ? nullptr : &*it;
}

ChrBounds compute_bounds(const std::vector<ChrVertex>& vertices) {
    if (vertices.empty()) {
        return {};
    }
    ChrBounds bounds;
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

} // namespace

ChrInfo load_chr_info(const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    if (data.size() < 12 || bin::u32le(data, 0) != 0x30686373) {
        throw std::runtime_error("bad CHR file: " + path.string());
    }

    ChrInfo info;
    info.path = path;
    info.file_size = data.size();
    info.version_or_flags = bin::i32le(data, 4);
    info.chunk_count = bin::i32le(data, 8);
    if (info.chunk_count < 0) {
        throw std::runtime_error("negative CHR chunk count");
    }

    std::size_t cursor = 12;
    const auto chunk_count = static_cast<std::size_t>(info.chunk_count);
    bin::require_range(data, cursor, chunk_count * 12, "CHR chunk table");
    info.chunks.reserve(chunk_count);
    for (std::size_t i = 0; i < chunk_count; ++i) {
        ChrChunk chunk;
        chunk.type = bin::i32le(data, cursor + 0);
        chunk.offset = bin::u32le(data, cursor + 4);
        chunk.size = bin::u32le(data, cursor + 8);
        bin::require_range(data, chunk.offset, chunk.size, "CHR chunk payload");
        info.chunks.push_back(chunk);
        cursor += 12;
    }

    for (const auto& chunk : info.chunks) {
        if (chunk.type == 7) {
            read_bone_names(info, data, chunk);
        }
    }
    return info;
}

ChrMesh load_chr_mesh(const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    ChrMesh mesh;
    mesh.info = load_chr_info(path);

    const auto* vertex_chunk = find_chunk(mesh.info, 1);
    const auto* index_chunk = find_chunk(mesh.info, 2);
    if (!vertex_chunk || !index_chunk) {
        throw std::runtime_error("CHR mesh chunks missing: " + path.string());
    }
    if (vertex_chunk->size % 0x28 != 0 || index_chunk->size % 2 != 0) {
        throw std::runtime_error("bad CHR mesh chunk size: " + path.string());
    }

    const std::size_t vertex_count = vertex_chunk->size / 0x28;
    mesh.vertices.reserve(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const std::size_t offset = vertex_chunk->offset + i * 0x28;
        ChrVertex vertex;
        vertex.x = bin::f32le(data, offset + 0x00);
        vertex.y = bin::f32le(data, offset + 0x04);
        vertex.z = bin::f32le(data, offset + 0x08);
        vertex.nx = bin::f32le(data, offset + 0x0c);
        vertex.ny = bin::f32le(data, offset + 0x10);
        vertex.nz = bin::f32le(data, offset + 0x14);
        vertex.u = bin::f32le(data, offset + 0x18);
        vertex.v = bin::f32le(data, offset + 0x1c);
        vertex.bone0 = bin::u8(data, offset + 0x20);
        vertex.bone1 = bin::u8(data, offset + 0x21);
        vertex.blend = bin::f32le(data, offset + 0x24);
        mesh.vertices.push_back(vertex);
    }

    const std::size_t index_count = index_chunk->size / 2;
    mesh.indices.reserve(index_count);
    for (std::size_t i = 0; i < index_count; ++i) {
        const auto index = bin::u16le(data, index_chunk->offset + i * 2);
        if (index >= mesh.vertices.size()) {
            throw std::runtime_error("CHR index out of vertex range: " + path.string());
        }
        mesh.indices.push_back(index);
    }

    mesh.bounds = compute_bounds(mesh.vertices);
    return mesh;
}

} // namespace sphere::model
