#pragma once

#include <array>

namespace sphere::client {

struct CharacterCameraProfile {
    bool valid = false;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float target_z = 0.0f;
    float yaw = 0.0f;
    float distance = 1.0f;
    float pitch = 0.0f;
    float fov_degrees = 50.0f;
};

using CharacterCameraProfileTable = std::array<CharacterCameraProfile, 17>;

} // namespace sphere::client
