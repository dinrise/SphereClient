#pragma once

#include "client/character_camera.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct lua_State;

namespace sphere::client {

struct LuaAppearanceGenderConfig {
    int face_count = 0;
    int hair_count = 0;
};

struct LuaAppearanceConfig {
    bool ok = false;
    int model_base = 0;
    LuaAppearanceGenderConfig male;
    LuaAppearanceGenderConfig female;
    int hair_color_count = 0;
    int tattoo_count = 0;
    CharacterCameraProfileTable camera_profiles{};
};

struct LuaGrassSampleOffset {
    float x = 0.0f;
    float z = 0.0f;
};

struct LuaGameWindowConfig {
    bool ok = false;
    std::vector<std::wstring> ui_windows;
    std::vector<std::wstring> settings_windows;
    std::wstring map_file;
    std::vector<std::wstring> landscape_dirs;
    std::vector<std::wstring> model_dirs;
    std::vector<std::wstring> static_object_dirs;
    std::wstring grassmap_dir;
    std::array<std::vector<std::wstring>, 16> grass_patterns{};
    std::vector<std::wstring> grass_detail_models;
    std::vector<LuaGrassSampleOffset> grass_sample_offsets;
    std::wstring terrain_microtexture;
    int grid_width = 0;
    int origin_row = 0;
    int origin_column = 0;
    int visible_radius = 0;
    float tile_size = 0.0f;
    float static_object_radius = 0.0f;
    int grassmap_grid_size = 0;
    int grassmap_tile_resolution = 0;
    int grassmap_world_offset = 0;
    int grass_quality = 0;
    float grass_radius = 0.0f;
    float grass_spacing = 0.0f;
    int grass_detail_count = 0;
    float grass_jitter_fraction = 0.0f;
    float grass_scale_min = 0.0f;
    float grass_scale_max = 0.0f;
    float grass_flatness_radius = 0.0f;
    float grass_flatness_threshold = 0.0f;
    float grass_generation_margin = 0.0f;
    float grass_wind_amplitude = 0.0f;
    float grass_wind_speed = 0.0f;
    std::wstring camera_mode;
    float camera_eye_height = 0.0f;
    float camera_look_distance = 0.0f;
    float camera_turn_speed = 0.0f;
    float camera_pitch_speed = 0.0f;
    float camera_min_pitch = 0.0f;
    float camera_max_pitch = 0.0f;
    float camera_fov = 0.0f;
    float walk_speed = 0.0f;
    float run_multiplier = 0.0f;
    float player_collision_radius = 0.0f;
    float player_collision_height = 0.0f;
    float max_step_height = 0.0f;
    float movement_collision_step = 0.0f;
    float collision_floor_normal_threshold = 0.0f;
    int position_send_interval_ms = 0;
    float near_clip = 0.0f;
    float far_clip = 0.0f;
    float fog_start = 0.0f;
    float fog_end = 0.0f;
    int clear_red = 0;
    int clear_green = 0;
    int clear_blue = 0;
};

struct LuaBootResult {
    bool ok = false;
    std::wstring message;
    int main_exports = 0;
    int module_count = 0;
    LuaAppearanceConfig appearance;
    LuaGameWindowConfig game_window;
};

class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    LuaBootResult initialize(const std::filesystem::path& root);

private:
    lua_State* state_ = nullptr;
    int main_module_ref_ = -2;
};

} // namespace sphere::client
