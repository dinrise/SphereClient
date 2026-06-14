#include "client/bitmap_font.hpp"

#include "common/binary_reader.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sphere::client {
namespace {

std::string read_c_string(const bin::ByteBuffer& data, std::size_t& offset) {
    return bin::read_c_string(data, offset);
}

float f32le(const bin::ByteBuffer& data, std::size_t offset) {
    const auto raw = bin::u32le(data, offset);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::vector<std::uint8_t> to_cp1251(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(1251, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, "?", nullptr);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(needed));
    if (needed > 0) {
        WideCharToMultiByte(1251, 0, text.c_str(), static_cast<int>(text.size()), reinterpret_cast<char*>(out.data()), needed, "?", nullptr);
    }
    return out;
}

void alpha_blit(HDC dc, const BitmapImage& image, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    HDC mem_dc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem_dc, image.handle);
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(dc, dx, dy, dw, dh, mem_dc, sx, sy, sw, sh, blend);
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
}

BitmapImage make_tinted_texture(const BitmapImage& source, COLORREF color, BYTE alpha) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source.width;
    info.bmiHeader.biHeight = -source.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels || !source.pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return {};
    }

    const unsigned r = GetRValue(color);
    const unsigned g = GetGValue(color);
    const unsigned b = GetBValue(color);
    const unsigned a = alpha;
    auto* dst = static_cast<std::uint8_t*>(pixels);
    const int dst_stride = source.width * 4;
    for (int y = 0; y < source.height; ++y) {
        const auto* row_src = source.pixels + static_cast<std::size_t>(y) * source.stride;
        auto* row_dst = dst + static_cast<std::size_t>(y) * dst_stride;
        for (int x = 0; x < source.width; ++x) {
            const auto* src_px = row_src + x * 4;
            auto* dst_px = row_dst + x * 4;
            dst_px[0] = static_cast<std::uint8_t>((static_cast<unsigned>(src_px[0]) * b * a) / (255U * 255U));
            dst_px[1] = static_cast<std::uint8_t>((static_cast<unsigned>(src_px[1]) * g * a) / (255U * 255U));
            dst_px[2] = static_cast<std::uint8_t>((static_cast<unsigned>(src_px[2]) * r * a) / (255U * 255U));
            dst_px[3] = static_cast<std::uint8_t>((static_cast<unsigned>(src_px[3]) * a) / 255U);
        }
    }

    BitmapImage image;
    image.handle = bitmap;
    image.pixels = dst;
    image.width = source.width;
    image.height = source.height;
    image.stride = dst_stride;
    image.has_alpha = source.has_alpha;
    return image;
}

} // namespace

BitmapFont BitmapFont::load(const std::filesystem::path& root, const std::wstring& font_name) {
    const auto sfn_path = root / "effects" / (font_name + L".sfn");
    const auto data = bin::read_file(sfn_path);
    if (data.size() < 40 || data[0] != 'S' || data[1] != 'F' || data[2] != 'N' || data[3] != 'T') {
        throw std::runtime_error("bad SFN file: " + sfn_path.string());
    }

    std::size_t offset = 4;
    read_c_string(data, offset);
    read_c_string(data, offset);
    const auto line_height = static_cast<int>(bin::u32le(data, offset));
    const auto baseline = static_cast<int>(bin::u32le(data, offset + 4));
    offset += 8;
    if (data.size() < offset + 224 * 28) {
        throw std::runtime_error("truncated SFN glyph table: " + sfn_path.string());
    }

    BitmapFont font;
    font.line_height_ = line_height;
    font.baseline_ = baseline;
    font.texture_ = load_dds_rgb_bitmap(root / "xadd" / (font_name + L".dds"));

    for (std::size_t i = 0; i < font.glyphs_.size(); ++i) {
        const std::size_t rec = offset + i * 28;
        BitmapGlyph glyph;
        const int cell_width = static_cast<int>(bin::i16le(data, rec));
        const int visible_width = static_cast<int>(bin::i16le(data, rec + 8));
        glyph.advance = visible_width > 0 ? visible_width : cell_width;
        glyph.height = static_cast<int>(bin::i16le(data, rec + 2));
        glyph.x_offset = static_cast<int>(bin::i16le(data, rec + 4));
        glyph.y_offset = static_cast<int>(bin::i16le(data, rec + 6));

        const float u1 = f32le(data, rec + 12);
        const float v1 = f32le(data, rec + 16);
        const float u2 = f32le(data, rec + 20);
        const float v2 = f32le(data, rec + 24);
        glyph.source_x = static_cast<int>(std::lround(u1 * font.texture_.width));
        glyph.source_y = static_cast<int>(std::lround(v1 * font.texture_.height));
        glyph.source_w = static_cast<int>(std::lround((u2 - u1) * font.texture_.width));
        glyph.source_h = static_cast<int>(std::lround((v2 - v1) * font.texture_.height));
        font.glyphs_[i] = glyph;
    }

    return font;
}

int BitmapFont::measure_text(const std::wstring& text) const {
    int width = 0;
    for (std::uint8_t ch : to_cp1251(text)) {
        if (ch < 32) {
            continue;
        }
        width += glyphs_[ch - 32].advance;
    }
    return width;
}

const BitmapImage& BitmapFont::texture_for_color(COLORREF color, BYTE alpha) const {
    if (color == RGB(255, 255, 255) && alpha == 255) {
        return texture_;
    }
    const auto key = static_cast<std::uint32_t>(alpha) << 24 |
                     static_cast<std::uint32_t>(GetRValue(color)) << 16 |
                     static_cast<std::uint32_t>(GetGValue(color)) << 8 |
                     static_cast<std::uint32_t>(GetBValue(color));
    auto it = tinted_textures_.find(key);
    if (it != tinted_textures_.end() && it->second) {
        return it->second;
    }
    auto [inserted, _] = tinted_textures_.emplace(key, make_tinted_texture(texture_, color, alpha));
    if (inserted->second) {
        return inserted->second;
    }
    return texture_;
}

void BitmapFont::draw_text(HDC dc, const std::wstring& text, RECT rect, UINT format, COLORREF color, BYTE alpha) const {
    if (!valid() || text.empty()) {
        return;
    }

    const auto bytes = to_cp1251(text);
    const auto& texture = texture_for_color(color, alpha);
    const int max_width = max(1, rect.right - rect.left);
    const bool single_line = (format & DT_SINGLELINE) != 0;
    const bool word_break = !single_line && (format & DT_WORDBREAK) != 0;
    const auto advance = [&](std::uint8_t ch) {
        return ch < 32 ? 0 : glyphs_[ch - 32].advance;
    };

    std::vector<std::vector<std::uint8_t>> lines(1);
    std::vector<int> widths(1, 0);
    const auto new_line = [&]() {
        lines.emplace_back();
        widths.push_back(0);
    };
    const auto append_char = [&](std::uint8_t ch) {
        if (!single_line && !lines.back().empty() && widths.back() + advance(ch) > max_width) {
            new_line();
        }
        lines.back().push_back(ch);
        widths.back() += advance(ch);
    };

    for (std::size_t index = 0; index < bytes.size();) {
        const auto ch = bytes[index];
        if (ch == '\r') {
            ++index;
            continue;
        }
        if (ch == '\n') {
            if (single_line) {
                break;
            }
            new_line();
            ++index;
            continue;
        }
        if (word_break && ch == ' ') {
            if (!lines.back().empty() && widths.back() + advance(ch) <= max_width) {
                lines.back().push_back(ch);
                widths.back() += advance(ch);
            }
            ++index;
            continue;
        }
        if (!word_break) {
            append_char(ch);
            ++index;
            continue;
        }

        std::size_t end = index;
        int word_width = 0;
        while (end < bytes.size() && bytes[end] != ' ' && bytes[end] != '\r' && bytes[end] != '\n') {
            word_width += advance(bytes[end]);
            ++end;
        }
        if (!lines.back().empty() && widths.back() + word_width > max_width) {
            new_line();
        }
        while (index < end) {
            append_char(bytes[index++]);
        }
    }
    if (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
        widths.pop_back();
    }

    const int line_step = line_height_ + 2;
    int y = rect.top;
    if ((format & DT_VCENTER) != 0) {
        y += max(0, ((rect.bottom - rect.top) - static_cast<int>(lines.size()) * line_step) / 2);
    }
    for (std::size_t line_index = 0; line_index < lines.size() && y <= rect.bottom; ++line_index) {
        int x = rect.left;
        if ((format & DT_CENTER) != 0) {
            x += max(0, (max_width - widths[line_index]) / 2);
        } else if ((format & DT_RIGHT) != 0) {
            x = rect.right - widths[line_index];
        }
        for (const auto ch : lines[line_index]) {
            if (ch < 32) {
                continue;
            }
            const auto& glyph = glyphs_[ch - 32];
            if (glyph.source_w > 0 && glyph.source_h > 0 && ch != 32) {
                const int dx = x + glyph.x_offset;
                const int dy = y + (baseline_ - glyph.y_offset);
                alpha_blit(dc, texture, dx, dy, glyph.source_w, glyph.source_h, glyph.source_x, glyph.source_y, glyph.source_w, glyph.source_h);
            }
            x += glyph.advance;
        }
        y += line_step;
    }
}

} // namespace sphere::client
