#include "client/dds_bitmap.hpp"

#include "common/binary_reader.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace sphere::client {

BitmapImage::BitmapImage(BitmapImage&& other) noexcept {
    handle = other.handle;
    pixels = other.pixels;
    width = other.width;
    height = other.height;
    stride = other.stride;
    has_alpha = other.has_alpha;
    other.handle = nullptr;
    other.pixels = nullptr;
    other.width = 0;
    other.height = 0;
    other.stride = 0;
    other.has_alpha = false;
}

BitmapImage& BitmapImage::operator=(BitmapImage&& other) noexcept {
    if (this != &other) {
        reset();
        handle = other.handle;
        pixels = other.pixels;
        width = other.width;
        height = other.height;
        stride = other.stride;
        has_alpha = other.has_alpha;
        other.handle = nullptr;
        other.pixels = nullptr;
        other.width = 0;
        other.height = 0;
        other.stride = 0;
        other.has_alpha = false;
    }
    return *this;
}

BitmapImage::~BitmapImage() {
    reset();
}

void BitmapImage::reset() {
    if (handle) {
        DeleteObject(handle);
        handle = nullptr;
    }
    pixels = nullptr;
    width = 0;
    height = 0;
    stride = 0;
    has_alpha = false;
}

BitmapImage load_dds_rgb_bitmap(const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    if (data.size() < 128 || data[0] != 'D' || data[1] != 'D' || data[2] != 'S' || data[3] != ' ') {
        throw std::runtime_error("not a DDS file: " + path.string());
    }

    const auto header_size = bin::u32le(data, 4);
    const auto height = static_cast<int>(bin::u32le(data, 12));
    const auto width = static_cast<int>(bin::u32le(data, 16));
    const auto pixel_format_size = bin::u32le(data, 76);
    const auto pixel_format_flags = bin::u32le(data, 80);
    const auto rgb_bit_count = bin::u32le(data, 88);
    const auto r_mask = bin::u32le(data, 92);
    const auto g_mask = bin::u32le(data, 96);
    const auto b_mask = bin::u32le(data, 100);
    const auto a_mask = bin::u32le(data, 104);

    if (header_size != 124 || pixel_format_size != 32 || width <= 0 || height <= 0) {
        throw std::runtime_error("unsupported DDS header: " + path.string());
    }
    if ((pixel_format_flags & 0x40U) == 0 || r_mask != 0x00FF0000U || g_mask != 0x0000FF00U || b_mask != 0x000000FFU) {
        throw std::runtime_error("unsupported DDS pixel format: " + path.string());
    }
    if (rgb_bit_count != 24 && rgb_bit_count != 32) {
        throw std::runtime_error("only uncompressed RGB24/RGB32 DDS is supported: " + path.string());
    }
    const bool has_alpha = rgb_bit_count == 32 && (pixel_format_flags & 0x1U) != 0 && a_mask == 0xFF000000U;

    const std::size_t bytes_per_pixel = rgb_bit_count / 8;
    const std::size_t source_stride = static_cast<std::size_t>(width) * bytes_per_pixel;
    const std::size_t image_size = source_stride * static_cast<std::size_t>(height);
    bin::require_range(data, 128, image_size, "DDS top mip level");

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        throw std::runtime_error("CreateDIBSection failed for DDS: " + path.string());
    }

    const auto* src = data.data() + 128;
    auto* dst = static_cast<std::uint8_t*>(pixels);
    const std::size_t dst_stride = static_cast<std::size_t>(width) * 4;
    if (rgb_bit_count == 24) {
        for (int y = 0; y < height; ++y) {
            const auto* row_src = src + static_cast<std::size_t>(y) * source_stride;
            auto* row_dst = dst + static_cast<std::size_t>(y) * dst_stride;
            for (int x = 0; x < width; ++x) {
                row_dst[x * 4 + 0] = row_src[x * 3 + 0];
                row_dst[x * 4 + 1] = row_src[x * 3 + 1];
                row_dst[x * 4 + 2] = row_src[x * 3 + 2];
                row_dst[x * 4 + 3] = 255;
            }
        }
    } else {
        for (int y = 0; y < height; ++y) {
            const auto* row_src = src + static_cast<std::size_t>(y) * source_stride;
            auto* row_dst = dst + static_cast<std::size_t>(y) * dst_stride;
            for (int x = 0; x < width; ++x) {
                const std::uint8_t alpha = has_alpha ? row_src[x * 4 + 3] : 255;
                row_dst[x * 4 + 0] = static_cast<std::uint8_t>((static_cast<unsigned>(row_src[x * 4 + 0]) * alpha) / 255);
                row_dst[x * 4 + 1] = static_cast<std::uint8_t>((static_cast<unsigned>(row_src[x * 4 + 1]) * alpha) / 255);
                row_dst[x * 4 + 2] = static_cast<std::uint8_t>((static_cast<unsigned>(row_src[x * 4 + 2]) * alpha) / 255);
                row_dst[x * 4 + 3] = alpha;
            }
        }
    }

    BitmapImage image;
    image.handle = bitmap;
    image.pixels = static_cast<std::uint8_t*>(pixels);
    image.width = width;
    image.height = height;
    image.stride = static_cast<int>(dst_stride);
    image.has_alpha = has_alpha;
    return image;
}

} // namespace sphere::client
