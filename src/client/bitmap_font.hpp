#pragma once

#include "client/dds_bitmap.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace sphere::client {

struct BitmapGlyph {
    int advance = 0;
    int height = 0;
    int x_offset = 0;
    int y_offset = 0;
    int source_x = 0;
    int source_y = 0;
    int source_w = 0;
    int source_h = 0;
};

class BitmapFont {
public:
    BitmapFont() = default;
    BitmapFont(const BitmapFont&) = delete;
    BitmapFont& operator=(const BitmapFont&) = delete;
    BitmapFont(BitmapFont&&) noexcept = default;
    BitmapFont& operator=(BitmapFont&&) noexcept = default;

    static BitmapFont load(const std::filesystem::path& root, const std::wstring& font_name);

    bool valid() const { return texture_ && line_height_ > 0; }
    int line_height() const { return line_height_; }
    int measure_text(const std::wstring& text) const;
    void draw_text(HDC dc, const std::wstring& text, RECT rect, UINT format, COLORREF color = RGB(255, 255, 255), BYTE alpha = 255) const;

private:
    const BitmapImage& texture_for_color(COLORREF color, BYTE alpha) const;

    int line_height_ = 0;
    int baseline_ = 0;
    BitmapImage texture_;
    std::array<BitmapGlyph, 224> glyphs_{};
    mutable std::unordered_map<std::uint32_t, BitmapImage> tinted_textures_;
};

} // namespace sphere::client
