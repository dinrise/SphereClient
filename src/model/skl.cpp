#include "model/skl.hpp"

#include "common/binary_reader.hpp"

#include <stdexcept>

namespace sphere::model {
namespace {

std::string read_fixed_name(const bin::ByteBuffer& data, std::size_t offset, std::size_t size) {
    bin::require_range(data, offset, size, "SKL bone name");
    std::size_t length = 0;
    while (length < size && data[offset + length] != 0) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(data.data() + offset), length);
}

} // namespace

SklSkeleton load_skl_skeleton(const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    if (data.size() < 8) {
        throw std::runtime_error("bad SKL file: " + path.string());
    }

    SklSkeleton skeleton;
    skeleton.path = path;
    skeleton.bone_count = bin::i32le(data, 0);
    skeleton.frame_count = bin::i32le(data, 4);
    if (skeleton.bone_count <= 0 || skeleton.frame_count <= 0) {
        throw std::runtime_error("bad SKL counts: " + path.string());
    }

    std::size_t cursor = 8;
    const auto bone_count = static_cast<std::size_t>(skeleton.bone_count);
    const auto frame_count = static_cast<std::size_t>(skeleton.frame_count);

    bin::require_range(data, cursor, bone_count * 4, "SKL parent table");
    skeleton.parents.reserve(bone_count);
    for (std::size_t i = 0; i < bone_count; ++i) {
        const auto parent = bin::i32le(data, cursor + i * 4);
        if (parent < -1 || parent >= skeleton.bone_count) {
            throw std::runtime_error("SKL parent index out of range: " + path.string());
        }
        skeleton.parents.push_back(parent);
    }
    cursor += bone_count * 4;

    bin::require_range(data, cursor, bone_count * 0x1e, "SKL bone names");
    skeleton.bone_names.reserve(bone_count);
    for (std::size_t i = 0; i < bone_count; ++i) {
        skeleton.bone_names.push_back(read_fixed_name(data, cursor + i * 0x1e, 0x1e));
    }
    cursor += bone_count * 0x1e;

    const std::size_t transform_count = bone_count * frame_count;
    bin::require_range(data, cursor, transform_count * 0x1c, "SKL transform tracks");
    skeleton.transforms.reserve(transform_count);
    for (std::size_t i = 0; i < transform_count; ++i) {
        const std::size_t offset = cursor + i * 0x1c;
        SklTransform transform;
        transform.qx = bin::f32le(data, offset + 0x00);
        transform.qy = bin::f32le(data, offset + 0x04);
        transform.qz = bin::f32le(data, offset + 0x08);
        transform.qw = bin::f32le(data, offset + 0x0c);
        transform.tx = bin::f32le(data, offset + 0x10);
        transform.ty = bin::f32le(data, offset + 0x14);
        transform.tz = bin::f32le(data, offset + 0x18);
        skeleton.transforms.push_back(transform);
    }
    cursor += transform_count * 0x1c;

    bin::require_range(data, cursor, 4, "SKL animation count");
    const auto animation_count = bin::i32le(data, cursor);
    cursor += 4;
    if (animation_count <= 0) {
        throw std::runtime_error("bad SKL animation count: " + path.string());
    }

    bin::require_range(data, cursor, static_cast<std::size_t>(animation_count) * 4, "SKL animation frame counts");
    skeleton.animation_frame_counts.reserve(static_cast<std::size_t>(animation_count));
    std::int32_t total_animation_frames = 0;
    for (std::int32_t i = 0; i < animation_count; ++i) {
        const auto frames = bin::i32le(data, cursor + static_cast<std::size_t>(i) * 4);
        if (frames <= 0) {
            throw std::runtime_error("bad SKL animation frame count: " + path.string());
        }
        total_animation_frames += frames;
        skeleton.animation_frame_counts.push_back(frames);
    }
    if (total_animation_frames > skeleton.frame_count) {
        throw std::runtime_error("SKL animation frames exceed track table: " + path.string());
    }

    return skeleton;
}

} // namespace sphere::model
