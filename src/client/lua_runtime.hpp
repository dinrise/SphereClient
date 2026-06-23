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

struct LuaSkyState {
    float time = 0.0f;
    int clear_red = 0;
    int clear_green = 0;
    int clear_blue = 0;
    int ambient_red = 0;
    int ambient_green = 0;
    int ambient_blue = 0;
    int sun_red = 0;
    int sun_green = 0;
    int sun_blue = 0;
    int cloud_red = 0;
    int cloud_green = 0;
    int cloud_blue = 0;
};

struct LuaUiAction {
    std::wstring window;
    int control = 0;
    std::wstring action;
    std::wstring target;
    std::wstring alternate;
};

struct LuaUiControlState {
    std::wstring window;
    int control = 0;
};

struct LuaGameWindowConfig {
    bool ok = false;
    std::vector<std::wstring> ui_windows;
    std::vector<std::wstring> settings_windows;
    std::vector<std::wstring> ui_initially_visible;
    std::vector<LuaUiAction> ui_actions;
    std::vector<LuaUiControlState> ui_initially_checked;
    std::wstring map_file;
    std::vector<std::wstring> landscape_dirs;
    std::vector<std::wstring> model_dirs;
    std::vector<std::wstring> static_object_dirs;
    std::wstring grassmap_dir;
    std::array<std::vector<std::wstring>, 31> grass_patterns{};
    std::array<std::vector<std::wstring>, 31> grass_flower_patterns{};  // slots 5-9 per pattern (FUN_00460570), "flower"+suffix
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
    float grassmap_world_offset_x = 0.0f;
    float grassmap_world_offset_z = 0.0f;
    float grassmap_world_scale = 0.0f;
    int grassmap_world_sign_x = 0;
    int grassmap_world_sign_z = 0;
    float grass_highland_min_y = 0.0f;
    float grass_highland_max_y = 0.0f;
    int grass_highland_pattern_offset = 0;
    int grass_quality = 0;
    float grass_radius = 0.0f;
    float grass_spacing = 0.0f;
    int grass_detail_count = 0;
    int grass_flower_count_max = 0;  // max flowers per all-flat cell (FUN_0047a150: (rand*0x14)>>15)
    float grass_jitter_fraction = 0.0f;
    float grass_scale_min = 0.0f;
    float grass_scale_max = 0.0f;
    float grass_flatness_radius = 0.0f;
    float grass_flatness_threshold = 0.0f;
    float grass_flatness_normal_y = 0.0f;
    float grass_generation_margin = 0.0f;
    float grass_wind_amplitude = 0.0f;
    float grass_wind_speed = 0.0f;
    // Tree/foliage sway (tree-wind VS): amplitude in world units at the leaf tips.
    float tree_wind_amplitude = 0.0f;
    float tree_wind_speed = 0.0f;
    // Grass "glow": the original lights grass with its own bright material rig
    // (FUN_00469090 sets fixed light dirs/colours into the u_grass material), and
    // the per-cell colour is brightened through per-channel LUTs (FUN_0047a150 +0x134),
    // so grass stays luminous independent of the dim time-of-day sun.
    float grass_glow = 0.0f;        // self-illumination floor (0..1): min brightness regardless of sun
    float grass_color_gain = 1.0f;  // brighten gain applied to the baked ground-colour tint
    // Distance grow/dissolve (FUN_00477020): grass grows from the ground + alpha-fades
    // between these distances (just inside the generation radius) so it appears/recedes
    // smoothly instead of popping when a new cell batch is baked.
    float grass_fade_start = 0.0f;  // within this distance: full height/opacity
    float grass_fade_end = 0.0f;    // beyond this distance: collapsed to ground + invisible
    // WindCircle gusts (native VS04 field): grass is pressed hard inside the 6 drifting
    // circles, calm elsewhere. grass_gust_radius_scale = 1/radius (0.2 native ≈ radius 5u;
    // smaller = bigger visible circles); grass_breeze = calm ever-present sway amount.
    float grass_gust_radius_scale = 0.2f;
    float grass_breeze = 0.3f;
    std::wstring camera_mode;
    std::wstring sky_texture;
    float sky_radius = 0.0f;
    float sky_height_scale = 0.0f;
    float sky_scroll_speed = 0.0f;
    int sky_red = 0;
    int sky_green = 0;
    int sky_blue = 0;
    std::vector<LuaSkyState> sky_states;
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
    float slope_slide_normal_y = 0.0f;  // floors below this |normal.y| slide downhill under gravity
    float slope_slide_factor = 0.0f;
    float jump_impulse = 0.0f;   // vertical velocity on jump (ControlMove 0x28C = -5; +y is down)
    float jump_gravity = 0.0f;   // native physics gravity (FUN_004755e0 double @0x504248 = 9.8)
    // Water reflection strength vs time-of-day (decoded from FUN_004db5e0): reflect
    // amount = gradientCoeff * (night/transition multiplier). Day = 0 (pure colour),
    // peaks in dawn/dusk, moderate at deep night.
    float water_day_start = 0.34f;          // _DAT_0050a70c
    float water_day_end = 0.66f;            // _DAT_0050a708
    float water_night_before = 0.19f;       // _DAT_004ff5a0
    float water_night_after = 0.81f;        // (float)_DAT_0050a700
    float water_transition_width = 0.15f;   // _DAT_004fef38
    float water_reflect_night = 0.3f;       // _DAT_004fe9a0
    float water_reflect_transition = 0.5f;  // _DAT_004fe7f8
    // Wave animation (FUN_0046a070): vertexY = (sin(phase)+amp)*scale + baseY,
    // phase = worldX*(freq_x/cell_step) + worldZ*(freq_z/cell_step) + time*speed.
    float wave_amp = 1.0f;        // _DAT_004fe850
    float wave_scale = 0.12f;     // _DAT_00502910 (per-type wave height; type-1 value)
    float wave_freq_x = 3.93f;    // _DAT_00504040
    float wave_freq_z = 2.02f;    // _DAT_00504048
    float wave_cell_step = 8.33f; // _DAT_00504038 (= tile_size/12)
    float wave_speed = 1.5f;      // time→phase rad/s (orig = DAT_04eb9cd0*pi/16; counter rate not extracted → tunable)
    int water_reflection_enabled = 1;  // 0 = flat time-coloured water only (no planar reflection)
    // Water body colour (Fresnel gradient endpoints), matched to the original's
    // turquoise lake. deep = looking straight down, graze = toward the horizon.
    int water_deep_r = 30, water_deep_g = 95, water_deep_b = 105;
    int water_graze_r = 70, water_graze_g = 150, water_graze_b = 165;
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
