#include "client/lua_runtime.hpp"

#include "lua.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace sphere::client {
namespace {

std::wstring widen_ascii(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

std::string lua_file_name(const std::filesystem::path& path) {
    return path.generic_string();
}

std::wstring lua_stack_error(lua_State* state) {
    const char* error = lua_tostring(state, -1);
    return error ? widen_ascii(error) : L"unknown Lua error";
}

std::wstring lua_field_error(const char* path) {
    return L"lua\\appearance.lua invalid field: " + widen_ascii(path);
}

std::wstring lua_game_window_field_error(const char* path) {
    return L"lua\\game_window.lua invalid field: " + widen_ascii(path);
}

bool read_game_integer_field(lua_State* state, int table_index, const char* field, int min_value, int max_value, int& out, std::wstring& error);
bool read_game_string_field(lua_State* state, int table_index, const char* field, std::wstring& out, std::wstring& error);

bool read_integer_field(lua_State* state, int table_index, const char* field, int min_value, int max_value, int& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (!lua_isnumber(state, -1)) {
        lua_pop(state, 1);
        error = lua_field_error(field);
        return false;
    }
    const auto value = static_cast<int>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    if (value < min_value || value > max_value) {
        error = lua_field_error(field);
        return false;
    }
    out = value;
    return true;
}

bool read_number_field(lua_State* state, int table_index, const char* field, double min_value, double max_value, float& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (!lua_isnumber(state, -1)) {
        lua_pop(state, 1);
        error = lua_field_error(field);
        return false;
    }
    const double value = static_cast<double>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    if (value < min_value || value > max_value) {
        error = lua_field_error(field);
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool read_string_array_field(lua_State* state, int table_index, const char* field, std::vector<std::wstring>& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }

    const auto length = static_cast<int>(lua_rawlen(state, -1));
    if (length <= 0) {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }

    out.clear();
    out.reserve(static_cast<std::size_t>(length));
    for (int i = 1; i <= length; ++i) {
        lua_rawgeti(state, -1, i);
        const char* value = lua_tostring(state, -1);
        if (!value || value[0] == '\0') {
            lua_pop(state, 2);
            error = lua_game_window_field_error(field);
            return false;
        }
        out.push_back(widen_ascii(value));
        lua_pop(state, 1);
    }

    lua_pop(state, 1);
    return true;
}

bool read_optional_game_string_field(
    lua_State* state,
    int table_index,
    const char* field,
    std::wstring& out,
    std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        out.clear();
        return true;
    }
    const char* value = lua_tostring(state, -1);
    if (!value || value[0] == '\0') {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }
    out = widen_ascii(value);
    lua_pop(state, 1);
    return true;
}

bool read_ui_actions(lua_State* state, int table_index, std::vector<LuaUiAction>& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, "ui_actions");
    if (!lua_istable(state, -1) || lua_rawlen(state, -1) == 0) {
        lua_pop(state, 1);
        error = lua_game_window_field_error("ui_actions");
        return false;
    }

    const auto count = static_cast<int>(lua_rawlen(state, -1));
    out.clear();
    out.reserve(static_cast<std::size_t>(count));
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            error = lua_game_window_field_error("ui_actions");
            return false;
        }

        LuaUiAction value;
        const bool ok =
            read_game_string_field(state, -1, "window", value.window, error) &&
            read_game_integer_field(state, -1, "control", 0, 65535, value.control, error) &&
            read_game_string_field(state, -1, "action", value.action, error) &&
            read_optional_game_string_field(state, -1, "target", value.target, error) &&
            read_optional_game_string_field(state, -1, "alternate", value.alternate, error);
        lua_pop(state, 1);
        if (!ok) {
            lua_pop(state, 1);
            return false;
        }
        out.push_back(std::move(value));
    }
    lua_pop(state, 1);
    return true;
}

bool read_ui_control_states(
    lua_State* state,
    int table_index,
    const char* field,
    std::vector<LuaUiControlState>& out,
    std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }

    const auto count = static_cast<int>(lua_rawlen(state, -1));
    out.clear();
    out.reserve(static_cast<std::size_t>(count));
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            error = lua_game_window_field_error(field);
            return false;
        }
        LuaUiControlState value;
        const bool ok =
            read_game_string_field(state, -1, "window", value.window, error) &&
            read_game_integer_field(state, -1, "control", 0, 65535, value.control, error);
        lua_pop(state, 1);
        if (!ok) {
            lua_pop(state, 1);
            return false;
        }
        out.push_back(std::move(value));
    }
    lua_pop(state, 1);
    return true;
}

bool read_grass_patterns(lua_State* state, int table_index, std::array<std::vector<std::wstring>, 31>& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, "grass_patterns");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = lua_game_window_field_error("grass_patterns");
        return false;
    }
    for (int type = 1; type < static_cast<int>(out.size()); ++type) {
        lua_rawgeti(state, -1, type);
        if (!lua_istable(state, -1) || lua_rawlen(state, -1) == 0) {
            lua_pop(state, 2);
            error = lua_game_window_field_error("grass_patterns");
            return false;
        }
        auto& patterns = out[static_cast<std::size_t>(type)];
        patterns.clear();
        const auto count = static_cast<int>(lua_rawlen(state, -1));
        patterns.reserve(static_cast<std::size_t>(count));
        for (int index = 1; index <= count; ++index) {
            lua_rawgeti(state, -1, index);
            const char* value = lua_tostring(state, -1);
            if (!value || value[0] == '\0') {
                lua_pop(state, 3);
                error = lua_game_window_field_error("grass_patterns");
                return false;
            }
            patterns.push_back(widen_ascii(value));
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool read_game_integer_field(lua_State* state, int table_index, const char* field, int min_value, int max_value, int& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (!lua_isinteger(state, -1)) {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }
    const auto value = static_cast<int>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    if (value < min_value || value > max_value) {
        error = lua_game_window_field_error(field);
        return false;
    }
    out = value;
    return true;
}

bool read_game_number_field(lua_State* state, int table_index, const char* field, double min_value, double max_value, float& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    if (!lua_isnumber(state, -1)) {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }
    const double value = static_cast<double>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    if (value < min_value || value > max_value) {
        error = lua_game_window_field_error(field);
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool read_grass_sample_offsets(
    lua_State* state,
    int table_index,
    std::vector<LuaGrassSampleOffset>& out,
    std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, "grass_sample_offsets");
    if (!lua_istable(state, -1) || lua_rawlen(state, -1) == 0) {
        lua_pop(state, 1);
        error = lua_game_window_field_error("grass_sample_offsets");
        return false;
    }

    const auto count = static_cast<int>(lua_rawlen(state, -1));
    out.clear();
    out.reserve(static_cast<std::size_t>(count));
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            error = lua_game_window_field_error("grass_sample_offsets");
            return false;
        }
        LuaGrassSampleOffset offset;
        if (!read_game_number_field(state, -1, "x", 0.0, 100.0, offset.x, error) ||
            !read_game_number_field(state, -1, "z", 0.0, 100.0, offset.z, error)) {
            lua_pop(state, 2);
            return false;
        }
        out.push_back(offset);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool read_sky_states(lua_State* state, int table_index, std::vector<LuaSkyState>& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, "sky_states");
    if (!lua_istable(state, -1) || lua_rawlen(state, -1) < 2) {
        lua_pop(state, 1);
        error = lua_game_window_field_error("sky_states");
        return false;
    }
    const auto count = static_cast<int>(lua_rawlen(state, -1));
    out.clear();
    out.reserve(static_cast<std::size_t>(count));
    float previous_time = -1.0f;
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            error = lua_game_window_field_error("sky_states");
            return false;
        }
        LuaSkyState value;
        const bool ok =
            read_game_number_field(state, -1, "time", 0.0, 0.999999, value.time, error) &&
            read_game_integer_field(state, -1, "clear_red", 0, 255, value.clear_red, error) &&
            read_game_integer_field(state, -1, "clear_green", 0, 255, value.clear_green, error) &&
            read_game_integer_field(state, -1, "clear_blue", 0, 255, value.clear_blue, error) &&
            read_game_integer_field(state, -1, "ambient_red", 0, 255, value.ambient_red, error) &&
            read_game_integer_field(state, -1, "ambient_green", 0, 255, value.ambient_green, error) &&
            read_game_integer_field(state, -1, "ambient_blue", 0, 255, value.ambient_blue, error) &&
            read_game_integer_field(state, -1, "sun_red", 0, 255, value.sun_red, error) &&
            read_game_integer_field(state, -1, "sun_green", 0, 255, value.sun_green, error) &&
            read_game_integer_field(state, -1, "sun_blue", 0, 255, value.sun_blue, error) &&
            read_game_integer_field(state, -1, "cloud_red", 0, 255, value.cloud_red, error) &&
            read_game_integer_field(state, -1, "cloud_green", 0, 255, value.cloud_green, error) &&
            read_game_integer_field(state, -1, "cloud_blue", 0, 255, value.cloud_blue, error);
        lua_pop(state, 1);
        if (!ok || value.time <= previous_time) {
            lua_pop(state, 1);
            if (ok) {
                error = lua_game_window_field_error("sky_states");
            }
            return false;
        }
        previous_time = value.time;
        out.push_back(value);
    }
    lua_pop(state, 1);
    return true;
}

bool read_game_string_field(lua_State* state, int table_index, const char* field, std::wstring& out, std::wstring& error) {
    table_index = lua_absindex(state, table_index);
    lua_getfield(state, table_index, field);
    const char* value = lua_tostring(state, -1);
    if (!value || value[0] == '\0') {
        lua_pop(state, 1);
        error = lua_game_window_field_error(field);
        return false;
    }
    out = widen_ascii(value);
    lua_pop(state, 1);
    return true;
}

bool read_gender_config(lua_State* state, int appearance_index, const char* gender, LuaAppearanceGenderConfig& out, std::wstring& error) {
    appearance_index = lua_absindex(state, appearance_index);
    lua_getfield(state, appearance_index, "genders");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = lua_field_error("genders");
        return false;
    }

    lua_getfield(state, -1, gender);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        error = lua_field_error(gender);
        return false;
    }

    const bool ok =
        read_integer_field(state, -1, "face_count", 1, 255, out.face_count, error) &&
        read_integer_field(state, -1, "hair_count", 1, 255, out.hair_count, error);
    lua_pop(state, 2);
    return ok;
}

bool read_camera_profile(lua_State* state, int profiles_index, int focus_id, CharacterCameraProfile& out, std::wstring& error) {
    profiles_index = lua_absindex(state, profiles_index);
    lua_rawgeti(state, profiles_index, focus_id);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = lua_field_error("camera_profiles");
        return false;
    }

    const bool ok =
        read_number_field(state, -1, "target_x", -20.0, 20.0, out.target_x, error) &&
        read_number_field(state, -1, "target_y", -20.0, 20.0, out.target_y, error) &&
        read_number_field(state, -1, "target_z", -20.0, 20.0, out.target_z, error) &&
        read_number_field(state, -1, "yaw", -6.4, 6.4, out.yaw, error) &&
        read_number_field(state, -1, "distance", 0.1, 80.0, out.distance, error) &&
        read_number_field(state, -1, "pitch", -1.5, 1.5, out.pitch, error) &&
        read_number_field(state, -1, "fov", 10.0, 120.0, out.fov_degrees, error);
    lua_pop(state, 1);
    out.valid = ok;
    return ok;
}

bool read_camera_profiles(lua_State* state, int appearance_index, CharacterCameraProfileTable& out, std::wstring& error) {
    appearance_index = lua_absindex(state, appearance_index);
    lua_getfield(state, appearance_index, "camera_profiles");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = lua_field_error("camera_profiles");
        return false;
    }

    const int required_profiles[] = {0, 12, 13, 14, 15, 16};
    for (const int focus_id : required_profiles) {
        if (!read_camera_profile(state, -1, focus_id, out[static_cast<std::size_t>(focus_id)], error)) {
            lua_pop(state, 1);
            return false;
        }
    }
    lua_pop(state, 1);
    return true;
}

bool load_appearance_config(lua_State* state, LuaAppearanceConfig& out, std::wstring& error) {
    lua_getglobal(state, "require");
    lua_pushliteral(state, "appearance");
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        error = lua_stack_error(state);
        lua_pop(state, 1);
        return false;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = L"lua\\appearance.lua did not return a module table";
        return false;
    }

    const int appearance_index = lua_absindex(state, -1);
    const bool ok =
        read_integer_field(state, appearance_index, "model_base", 0, 255, out.model_base, error) &&
        read_integer_field(state, appearance_index, "hair_color_count", 1, 255, out.hair_color_count, error) &&
        read_integer_field(state, appearance_index, "tattoo_count", 1, 255, out.tattoo_count, error) &&
        read_gender_config(state, appearance_index, "male", out.male, error) &&
        read_gender_config(state, appearance_index, "female", out.female, error) &&
        read_camera_profiles(state, appearance_index, out.camera_profiles, error);
    lua_pop(state, 1);
    out.ok = ok;
    return ok;
}

bool load_game_window_config(lua_State* state, LuaGameWindowConfig& out, std::wstring& error) {
    lua_getglobal(state, "require");
    lua_pushliteral(state, "game_window");
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        error = lua_stack_error(state);
        lua_pop(state, 1);
        return false;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = L"lua\\game_window.lua did not return a module table";
        return false;
    }

    const int config_index = lua_absindex(state, -1);
    const bool ok =
        read_string_array_field(state, config_index, "ui_windows", out.ui_windows, error) &&
        read_string_array_field(state, config_index, "settings_windows", out.settings_windows, error) &&
        read_string_array_field(state, config_index, "ui_initially_visible", out.ui_initially_visible, error) &&
        read_ui_actions(state, config_index, out.ui_actions, error) &&
        read_ui_control_states(state, config_index, "ui_initially_checked", out.ui_initially_checked, error) &&
        read_game_string_field(state, config_index, "map_file", out.map_file, error) &&
        read_string_array_field(state, config_index, "landscape_dirs", out.landscape_dirs, error) &&
        read_string_array_field(state, config_index, "model_dirs", out.model_dirs, error) &&
        read_string_array_field(state, config_index, "static_object_dirs", out.static_object_dirs, error) &&
        read_game_string_field(state, config_index, "grassmap_dir", out.grassmap_dir, error) &&
        read_grass_patterns(state, config_index, out.grass_patterns, error) &&
        read_string_array_field(state, config_index, "grass_detail_models", out.grass_detail_models, error) &&
        read_grass_sample_offsets(state, config_index, out.grass_sample_offsets, error) &&
        read_game_string_field(state, config_index, "terrain_microtexture", out.terrain_microtexture, error) &&
        read_game_integer_field(state, config_index, "grid_width", 1, 1024, out.grid_width, error) &&
        read_game_integer_field(state, config_index, "origin_row", -1024, 1024, out.origin_row, error) &&
        read_game_integer_field(state, config_index, "origin_column", -1024, 1024, out.origin_column, error) &&
        read_game_integer_field(state, config_index, "visible_radius", 0, 20, out.visible_radius, error) &&
        read_game_number_field(state, config_index, "tile_size", 1.0, 10000.0, out.tile_size, error) &&
        read_game_number_field(state, config_index, "static_object_radius", 1.0, 10000.0, out.static_object_radius, error) &&
        read_game_integer_field(state, config_index, "grassmap_grid_size", 1, 256, out.grassmap_grid_size, error) &&
        read_game_integer_field(state, config_index, "grassmap_tile_resolution", 1, 4096, out.grassmap_tile_resolution, error) &&
        read_game_number_field(state, config_index, "grassmap_world_offset_x", -100000.0, 100000.0, out.grassmap_world_offset_x, error) &&
        read_game_number_field(state, config_index, "grassmap_world_offset_z", -100000.0, 100000.0, out.grassmap_world_offset_z, error) &&
        read_game_number_field(state, config_index, "grassmap_world_scale", 0.000001, 1000.0, out.grassmap_world_scale, error) &&
        read_game_integer_field(state, config_index, "grassmap_world_sign_x", -1, 1, out.grassmap_world_sign_x, error) &&
        read_game_integer_field(state, config_index, "grassmap_world_sign_z", -1, 1, out.grassmap_world_sign_z, error) &&
        read_game_number_field(state, config_index, "grass_highland_min_y", -10000.0, 10000.0, out.grass_highland_min_y, error) &&
        read_game_number_field(state, config_index, "grass_highland_max_y", -10000.0, 10000.0, out.grass_highland_max_y, error) &&
        read_game_integer_field(state, config_index, "grass_highland_pattern_offset", 0, 30, out.grass_highland_pattern_offset, error) &&
        read_game_number_field(state, config_index, "grass_radius", 1.0, 500.0, out.grass_radius, error) &&
        read_game_number_field(state, config_index, "grass_spacing", 0.25, 100.0, out.grass_spacing, error) &&
        read_game_integer_field(state, config_index, "grass_detail_count", 1, 64, out.grass_detail_count, error) &&
        read_game_number_field(state, config_index, "grass_jitter_fraction", 0.0, 0.5, out.grass_jitter_fraction, error) &&
        read_game_number_field(state, config_index, "grass_scale_min", 0.01, 10.0, out.grass_scale_min, error) &&
        read_game_number_field(state, config_index, "grass_scale_max", 0.01, 10.0, out.grass_scale_max, error) &&
        read_game_number_field(state, config_index, "grass_flatness_radius", 0.1, 20.0, out.grass_flatness_radius, error) &&
        read_game_number_field(state, config_index, "grass_flatness_threshold", 0.0, 20.0, out.grass_flatness_threshold, error) &&
        read_game_number_field(state, config_index, "grass_flatness_normal_y", 0.0, 1.0, out.grass_flatness_normal_y, error) &&
        read_game_number_field(state, config_index, "grass_generation_margin", 1.0, 100.0, out.grass_generation_margin, error) &&
        read_game_number_field(state, config_index, "grass_wind_amplitude", 0.0, 0.5, out.grass_wind_amplitude, error) &&
        read_game_number_field(state, config_index, "grass_wind_speed", 0.0, 20.0, out.grass_wind_speed, error) &&
        read_game_string_field(state, config_index, "sky_texture", out.sky_texture, error) &&
        read_game_number_field(state, config_index, "sky_radius", 10.0, 10000.0, out.sky_radius, error) &&
        read_game_number_field(state, config_index, "sky_height_scale", 0.1, 5.0, out.sky_height_scale, error) &&
        read_game_number_field(state, config_index, "sky_scroll_speed", -10.0, 10.0, out.sky_scroll_speed, error) &&
        read_game_integer_field(state, config_index, "sky_red", 0, 255, out.sky_red, error) &&
        read_game_integer_field(state, config_index, "sky_green", 0, 255, out.sky_green, error) &&
        read_game_integer_field(state, config_index, "sky_blue", 0, 255, out.sky_blue, error) &&
        read_sky_states(state, config_index, out.sky_states, error) &&
        read_game_string_field(state, config_index, "camera_mode", out.camera_mode, error) &&
        read_game_number_field(state, config_index, "camera_eye_height", 0.1, 20.0, out.camera_eye_height, error) &&
        read_game_number_field(state, config_index, "camera_look_distance", 0.1, 1000.0, out.camera_look_distance, error) &&
        read_game_number_field(state, config_index, "camera_turn_speed", 0.0001, 1.0, out.camera_turn_speed, error) &&
        read_game_number_field(state, config_index, "camera_pitch_speed", 0.0001, 1.0, out.camera_pitch_speed, error) &&
        read_game_number_field(state, config_index, "camera_min_pitch", -1.5, 1.5, out.camera_min_pitch, error) &&
        read_game_number_field(state, config_index, "camera_max_pitch", -1.5, 1.5, out.camera_max_pitch, error) &&
        read_game_number_field(state, config_index, "camera_fov", 10.0, 120.0, out.camera_fov, error) &&
        read_game_number_field(state, config_index, "walk_speed", 0.01, 1000.0, out.walk_speed, error) &&
        read_game_number_field(state, config_index, "run_multiplier", 1.0, 20.0, out.run_multiplier, error) &&
        read_game_number_field(state, config_index, "player_collision_radius", 0.05, 10.0, out.player_collision_radius, error) &&
        read_game_number_field(state, config_index, "player_collision_height", 0.1, 20.0, out.player_collision_height, error) &&
        read_game_number_field(state, config_index, "max_step_height", 0.01, 20.0, out.max_step_height, error) &&
        read_game_number_field(state, config_index, "movement_collision_step", 0.01, 10.0, out.movement_collision_step, error) &&
        read_game_number_field(state, config_index, "collision_floor_normal_threshold", 0.0, 1.0, out.collision_floor_normal_threshold, error) &&
        read_game_integer_field(state, config_index, "position_send_interval_ms", 10, 10000, out.position_send_interval_ms, error) &&
        read_game_number_field(state, config_index, "near_clip", 0.001, 100.0, out.near_clip, error) &&
        read_game_number_field(state, config_index, "far_clip", 10.0, 10000.0, out.far_clip, error) &&
        read_game_number_field(state, config_index, "fog_start", 0.0, 10000.0, out.fog_start, error) &&
        read_game_number_field(state, config_index, "fog_end", 1.0, 10000.0, out.fog_end, error) &&
        read_game_integer_field(state, config_index, "clear_red", 0, 255, out.clear_red, error) &&
        read_game_integer_field(state, config_index, "clear_green", 0, 255, out.clear_green, error) &&
        read_game_integer_field(state, config_index, "clear_blue", 0, 255, out.clear_blue, error);
    lua_pop(state, 1);
    if (!ok) {
        return false;
    }
    if (out.camera_mode != L"first_person") {
        error = L"lua\\game_window.lua camera_mode must be first_person";
        return false;
    }
    if (out.grass_detail_models.empty()) {
        error = L"lua\\game_window.lua grass_detail_models must not be empty";
        return false;
    }
    if (out.grass_scale_min > out.grass_scale_max) {
        error = L"lua\\game_window.lua grass_scale_min must not exceed grass_scale_max";
        return false;
    }
    if (out.near_clip >= out.far_clip || out.fog_start >= out.fog_end || out.fog_end > out.far_clip ||
        out.camera_min_pitch >= out.camera_max_pitch) {
        error = L"lua\\game_window.lua invalid world distance ordering";
        return false;
    }
    out.ok = true;
    return true;
}

int call_script_stub(lua_State* state) {
    const char* module = luaL_optstring(state, 1, "?");
    const char* function = luaL_optstring(state, 2, "?");
    luaL_error(state, "Lua script function is not implemented yet: %s.%s", module, function);
    return 0;
}

void install_sphere_runtime(lua_State* state) {
    lua_newtable(state); // sphere
    lua_newtable(state); // sphere.runtime
    lua_pushcfunction(state, call_script_stub);
    lua_setfield(state, -2, "call_script_stub");
    lua_setfield(state, -2, "runtime");
    lua_setglobal(state, "sphere");
}

int install_module_catalog(lua_State* state, const std::filesystem::path& lua_dir) {
    std::vector<std::filesystem::path> modules;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(lua_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
            continue;
        }
        const auto relative = std::filesystem::relative(entry.path(), lua_dir);
        if (!relative.empty() && relative.begin()->wstring() == L"_pseudo") {
            continue;
        }
        modules.push_back(relative);
    }
    std::sort(modules.begin(), modules.end());

    lua_getglobal(state, "sphere");
    lua_newtable(state);
    for (const auto& relative : modules) {
        auto module = relative;
        module.replace_extension();
        const auto module_name = module.generic_string();
        const auto module_path = relative.generic_string();
        lua_pushlstring(state, module_path.data(), module_path.size());
        lua_setfield(state, -2, module_name.c_str());
    }
    lua_setfield(state, -2, "modules");
    lua_pop(state, 1);
    return static_cast<int>(modules.size());
}

void prepend_package_path(lua_State* state, const std::filesystem::path& lua_dir) {
    const std::string prefix = lua_file_name(lua_dir / "?.lua") + ";" + lua_file_name(lua_dir / "?" / "init.lua") + ";";
    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    lua_getfield(state, -1, "path");
    const char* old_path = lua_tostring(state, -1);
    std::string next_path = prefix;
    if (old_path) {
        next_path += old_path;
    }
    lua_pop(state, 1);
    lua_pushstring(state, next_path.c_str());
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);
}

} // namespace

LuaRuntime::LuaRuntime() = default;

LuaRuntime::~LuaRuntime() {
    if (state_) {
        if (main_module_ref_ != LUA_NOREF) {
            luaL_unref(state_, LUA_REGISTRYINDEX, main_module_ref_);
        }
        lua_close(state_);
        state_ = nullptr;
    }
}

LuaBootResult LuaRuntime::initialize(const std::filesystem::path& root) {
    LuaBootResult result;
    const auto lua_dir = root / "lua";
    const auto main_path = lua_dir / "_main.lua";
    if (!std::filesystem::exists(main_path)) {
        result.message = L"lua\\_main.lua is missing";
        return result;
    }

    state_ = luaL_newstate();
    if (!state_) {
        result.message = L"luaL_newstate failed";
        return result;
    }

    luaL_openlibs(state_);
    install_sphere_runtime(state_);
    result.module_count = install_module_catalog(state_, lua_dir);
    prepend_package_path(state_, lua_dir);

    const auto main_file = lua_file_name(main_path);
    if (luaL_loadfile(state_, main_file.c_str()) != LUA_OK) {
        result.message = lua_stack_error(state_);
        lua_pop(state_, 1);
        return result;
    }
    if (lua_pcall(state_, 0, 1, 0) != LUA_OK) {
        result.message = lua_stack_error(state_);
        lua_pop(state_, 1);
        return result;
    }
    if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        result.message = L"lua\\_main.lua did not return a module table";
        return result;
    }

    lua_getfield(state_, -1, "__exports");
    if (lua_istable(state_, -1)) {
        result.main_exports = static_cast<int>(lua_rawlen(state_, -1));
    }
    lua_pop(state_, 1);

    std::wstring appearance_error;
    if (!load_appearance_config(state_, result.appearance, appearance_error)) {
        lua_pop(state_, 1);
        result.message = appearance_error;
        return result;
    }

    std::wstring game_window_error;
    if (!load_game_window_config(state_, result.game_window, game_window_error)) {
        lua_pop(state_, 1);
        result.message = game_window_error;
        return result;
    }

    main_module_ref_ = luaL_ref(state_, LUA_REGISTRYINDEX);
    result.ok = true;
    std::wostringstream message;
    message << L"Lua runtime loaded _main.lua; exports=" << result.main_exports
            << L"; modules=" << result.module_count
            << L"; appearance base=" << result.appearance.model_base
            << L"; game windows=" << result.game_window.ui_windows.size();
    result.message = message.str();
    return result;
}

} // namespace sphere::client
