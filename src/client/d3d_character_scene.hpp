#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "client/character_camera.hpp"
#include "client/character_render_mesh.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sphere::client {

class CharacterSelectScene {
public:
    CharacterSelectScene();
    ~CharacterSelectScene();

    CharacterSelectScene(const CharacterSelectScene&) = delete;
    CharacterSelectScene& operator=(const CharacterSelectScene&) = delete;

    bool initialize(HWND hwnd, const std::filesystem::path& root, std::wstring& error);
    void set_camera_profiles(const CharacterCameraProfileTable& profiles);
    bool set_overlay_bitmap(int width, int height, int x, int y, bool align_right_x, std::vector<std::uint8_t> bgra_pixels, std::wstring& error);
    bool set_character_gender(bool female, std::wstring& error);
    bool set_character_appearance(bool female, int face, int hair, int hair_color, int tattoo, std::wstring& error);
    CharacterRenderMesh character_render_mesh() const;
    void set_camera_focus(int focus_id);
    void rotate_character(float radians_delta);
    void resize();
    void render();
    bool valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sphere::client
