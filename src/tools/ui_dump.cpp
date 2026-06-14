#include "client/ui_definition.hpp"

#include <fcntl.h>
#include <io.h>

#include <exception>
#include <iostream>

namespace {

void print_color(const sphere::client::UiColor& color) {
    std::wcout << color.r << L"," << color.g << L"," << color.b << L"," << color.a;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    if (argc < 2) {
        std::wcerr << L"usage: ui_dump <file.ui>\n";
        return 2;
    }

    try {
        const auto window = sphere::client::load_ui_window(argv[1]);
        std::wcout << L"window " << window.name << L"\n";
        std::wcout << L"  text=" << window.text_key
                   << L" pos=" << window.x << L"," << window.y
                   << L" size=" << window.width << L"x" << window.height
                   << L" font=" << window.font_index
                   << L" alignRightX=" << (window.align_right_x ? L"true" : L"false")
                   << L"\n";
        std::wcout << L"  titleRect="
                   << window.title_left << L"," << window.title_top << L","
                   << window.title_right << L"," << window.title_bottom
                   << L" color=";
        print_color(window.text_color);
        std::wcout << L"\n";
        std::wcout << L"sprites " << window.sprites.size() << L"\n";
        for (const auto& [name, sprite] : window.sprites) {
            std::wcout << L"  sprite " << sprite.name
                       << L" key=" << name
                       << L" size=" << sprite.width << L"x" << sprite.height
                       << L" pieces=" << sprite.pieces.size()
                       << L"\n";
        }
        std::wcout << L"controls " << window.controls.size() << L"\n";
        for (const auto& control : window.controls) {
            std::wcout << L"  id=" << control.id
                       << L" class=" << control.class_id
                       << L" pos=" << control.x << L"," << control.y
                       << L" size=" << control.width << L"x" << control.height
                       << L" text=" << control.text_key
                       << L" image=" << control.image_name
                       << L" hidden=" << (control.hidden ? L"true" : L"false")
                       << L"\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
