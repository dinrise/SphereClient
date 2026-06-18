#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "client/character_render_mesh.hpp"
#include "client/lua_runtime.hpp"
#include "client/skinned_character.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sphere::client {

struct GameMovementInput {
    bool forward = false;
    bool backward = false;
    bool strafe_left = false;
    bool strafe_right = false;
    bool run = false;
};

struct GameWorldPosition {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double angle = 0.0;
};

class GameWorldScene {
public:
    GameWorldScene();
    ~GameWorldScene();

    GameWorldScene(const GameWorldScene&) = delete;
    GameWorldScene& operator=(const GameWorldScene&) = delete;

    bool initialize(
        HWND hwnd,
        const std::filesystem::path& root,
        const LuaGameWindowConfig& config,
        const SkinnedCharacterModel* player_model,
        double spawn_x,
        double spawn_y,
        double spawn_z,
        double spawn_angle,
        std::wstring& error);
    bool set_overlay_bitmap(int width, int height, std::vector<std::uint8_t> bgra_pixels, std::wstring& error);
    bool set_grass_quality(int quality, std::wstring& error);
    void set_fog(float start, float end);
    void set_game_time(float day_fraction);
    float current_game_time() const;
    float camera_facing() const;
    bool update(float delta_seconds, const GameMovementInput& input, std::wstring& error);
    void rotate_view(float mouse_dx, float mouse_dy);
    void jump();
    GameWorldPosition position() const;
    void resize();
    void render();
    bool valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sphere::client
