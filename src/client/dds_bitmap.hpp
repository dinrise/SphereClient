#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <filesystem>

namespace sphere::client {

struct BitmapImage {
    HBITMAP handle = nullptr;
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool has_alpha = false;

    BitmapImage() = default;
    BitmapImage(const BitmapImage&) = delete;
    BitmapImage& operator=(const BitmapImage&) = delete;
    BitmapImage(BitmapImage&& other) noexcept;
    BitmapImage& operator=(BitmapImage&& other) noexcept;
    ~BitmapImage();

    explicit operator bool() const { return handle != nullptr; }
    void reset();
};

BitmapImage load_dds_rgb_bitmap(const std::filesystem::path& path);

} // namespace sphere::client
