#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace sphere::client {

struct UiTexCoord {
    int u = 0;
    int v = 0;
};

struct UiSpritePiece {
    std::wstring texture_name;
    int src_left = 0;
    int src_top = 0;
    int src_right = 0;
    int src_bottom = 0;
    int dst_left = 0;
    int dst_top = 0;
    int dst_right = 0;
    int dst_bottom = 0;
    bool has_tcoords = false;
    std::array<UiTexCoord, 4> tcoords{};
};

struct UiSpriteDef {
    std::wstring name;
    int width = 0;
    int height = 0;
    std::vector<UiSpritePiece> pieces;
};

struct UiColor {
    int r = 255;
    int g = 255;
    int b = 255;
    int a = 255;
};

struct UiSubButtonDef {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::wstring checked_image;
    std::wstring focused_image;
    std::wstring disabled_image;
};

struct UiControlDef {
    int id = 0;
    std::wstring class_id;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int font_index = 0;
    UiColor text_color;
    UiColor disabled_color{128, 128, 128, 255};
    UiColor focus_color{255, 239, 212, 255};
    std::wstring text_key;
    std::wstring checked_image;
    std::wstring unchecked_image;
    std::wstring focused_image;
    std::wstring image_name;
    std::wstring draw_sprite_name;
    std::wstring scroll_sprite_name;
    int scroll_sprite_width = 0;
    int scroll_sprite_height = 0;
    int delta_step = 1;
    UiSubButtonDef left_button;
    UiSubButtonDef right_button;
    bool password = false;
    bool send_quit = false;
    bool hidden = false;
    bool disabled = false;
    bool text_center = false;
};

struct UiWindowDef {
    std::wstring name;
    std::wstring text_key;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int font_index = 0;
    UiColor text_color{237, 208, 161, 255};
    int title_left = 0;
    int title_top = 0;
    int title_right = 0;
    int title_bottom = 0;
    bool align_center_x = false;
    bool align_center_y = false;
    bool align_right_x = false;
    bool align_right_y = false;
    bool save_last_position = false;
    bool can_not_cross = false;
    bool can_go_top = true;
    bool draw_none = false;
    std::wstring draw_sprite_name;
    std::vector<UiControlDef> controls;
    std::unordered_map<std::wstring, UiSpriteDef> sprites;
};

using UiStringTable = std::unordered_map<std::wstring, std::wstring>;

std::wstring decode_cp1251_file(const std::filesystem::path& path);
UiStringTable load_ui_strings(const std::filesystem::path& path);
UiWindowDef load_ui_window(const std::filesystem::path& path);

const std::wstring& lookup_ui_string(const UiStringTable& strings, const std::wstring& key);

} // namespace sphere::client
