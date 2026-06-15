#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace sphere::client {

struct CharacterRenderVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct CharacterRenderBatch {
    std::uint32_t start_index = 0;
    std::uint32_t index_count = 0;
    std::filesystem::path texture_path;
    // Face/hair subobjects. Hidden when the local player is drawn in
    // first-person so the camera does not look at its own head.
    bool is_head = false;
};

struct CharacterRenderMesh {
    std::vector<CharacterRenderVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<CharacterRenderBatch> batches;

    bool valid() const {
        return !vertices.empty() && !indices.empty() && !batches.empty();
    }
};

} // namespace sphere::client
