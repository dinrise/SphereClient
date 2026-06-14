#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sphere::model {

struct SklTransform {
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float qw = 1.0f;
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
};

struct SklSkeleton {
    std::filesystem::path path;
    std::int32_t bone_count = 0;
    std::int32_t frame_count = 0;
    std::vector<std::int32_t> parents;
    std::vector<std::string> bone_names;
    std::vector<SklTransform> transforms;
    std::vector<std::int32_t> animation_frame_counts;
};

SklSkeleton load_skl_skeleton(const std::filesystem::path& path);

} // namespace sphere::model
