#include "client/ui_definition.hpp"

#include "common/binary_reader.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <stdexcept>

namespace sphere::client {
namespace {

std::wstring trim(std::wstring value) {
    auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](wchar_t ch) {
        return !is_space(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](wchar_t ch) {
        return !is_space(ch);
    }).base(), value.end());
    return value;
}

std::wstring strip_comment(std::wstring line) {
    bool in_quote = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] == L'"') {
            in_quote = !in_quote;
        }
        if (!in_quote && line[i] == L'/' && line[i + 1] == L'/') {
            line.resize(i);
            break;
        }
    }
    return line;
}

bool starts_with_token(const std::wstring& line, const wchar_t* token) {
    std::wistringstream input(line);
    std::wstring first;
    input >> first;
    return first == token;
}

bool is_open_brace(const std::wstring& line) {
    return line == L"{";
}

bool is_close_brace(const std::wstring& line) {
    return line == L"}";
}

std::wstring parse_quoted(const std::wstring& line) {
    const auto begin = line.find(L'"');
    if (begin == std::wstring::npos) {
        return {};
    }
    const auto end = line.find(L'"', begin + 1);
    if (end == std::wstring::npos) {
        return {};
    }
    return line.substr(begin + 1, end - begin - 1);
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool parse_bool(std::wistringstream& input, bool fallback = false) {
    std::wstring value;
    input >> value;
    value = lowercase(std::move(value));
    if (value == L"true" || value == L"1") {
        return true;
    }
    if (value == L"false" || value == L"0") {
        return false;
    }
    return fallback;
}

int parse_control_id_from_comment(const std::wstring& line) {
    const auto marker = line.find(L"id");
    if (marker == std::wstring::npos) {
        return 0;
    }
    const auto eq = line.find(L'=', marker);
    if (eq == std::wstring::npos) {
        return 0;
    }
    std::wistringstream input(line.substr(eq + 1));
    int id = 0;
    input >> id;
    return id;
}

UiColor parse_color(std::wistringstream& input, UiColor fallback) {
    UiColor color = fallback;
    input >> color.r >> color.g >> color.b >> color.a;
    return color;
}

std::vector<std::wstring> lines_of(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wistringstream input(text);
    std::wstring line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

std::wstring decode_cp1251_file(const std::filesystem::path& path) {
    const auto bytes = bin::read_file(path);
    if (bytes.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        1251,
        0,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (required <= 0) {
        throw std::runtime_error("failed to decode cp1251 file: " + path.string());
    }

    std::wstring out(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        1251,
        0,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        out.data(),
        required);
    return out;
}

UiStringTable load_ui_strings(const std::filesystem::path& path) {
    UiStringTable strings;
    const auto text = decode_cp1251_file(path);
    for (auto line : lines_of(text)) {
        line = trim(strip_comment(std::move(line)));
        if (!starts_with_token(line, L"string")) {
            continue;
        }

        std::wistringstream input(line);
        std::wstring keyword;
        std::wstring key;
        input >> keyword >> key;
        auto value = parse_quoted(line);
        if (!key.empty()) {
            strings[std::move(key)] = std::move(value);
        }
    }
    return strings;
}

UiWindowDef load_ui_window(const std::filesystem::path& path) {
    UiWindowDef window;
    const auto text = decode_cp1251_file(path);
    bool pending_sprite = false;
    bool in_sprite = false;
    UiSpriteDef sprite;
    bool pending_control = false;
    int pending_id = 0;
    bool in_control = false;
    int control_nested_depth = 0;
    std::wstring pending_control_section;
    std::wstring active_control_section;
    UiControlDef control;

    for (const auto& raw_line : lines_of(text)) {
        auto line = trim(strip_comment(raw_line));
        if (line.empty()) {
            continue;
        }

        if (!in_control && !in_sprite && starts_with_token(line, L"sprite")) {
            pending_sprite = true;
            continue;
        }

        if (!in_control && !in_sprite && raw_line.find(L"control") != std::wstring::npos) {
            const auto id = parse_control_id_from_comment(raw_line);
            pending_control = true;
            pending_id = id;
        }

        if (pending_sprite && is_open_brace(line)) {
            pending_sprite = false;
            in_sprite = true;
            sprite = UiSpriteDef{};
            continue;
        }

        if (in_sprite) {
            if (is_close_brace(line)) {
                if (!sprite.name.empty()) {
                    window.sprites[lowercase(sprite.name)] = std::move(sprite);
                }
                in_sprite = false;
                continue;
            }

            std::wistringstream input(line);
            std::wstring key;
            input >> key;
            if (key == L"name") {
                sprite.name = parse_quoted(line);
            } else if (key == L"size") {
                input >> sprite.width >> sprite.height;
            } else if (key == L"texture") {
                UiSpritePiece piece;
                piece.texture_name = parse_quoted(line);
                const auto end_quote = line.find(L'"', line.find(L'"') + 1);
                if (end_quote != std::wstring::npos) {
                    std::wistringstream coords(line.substr(end_quote + 1));
                    coords >> piece.src_left >> piece.src_top >> piece.src_right >> piece.src_bottom
                           >> piece.dst_left >> piece.dst_top >> piece.dst_right >> piece.dst_bottom;
                    sprite.pieces.push_back(std::move(piece));
                }
            } else if (key == L"tcoords") {
                int index = -1;
                input >> index;
                if (index >= 0 && index < static_cast<int>(sprite.pieces.size())) {
                    auto& piece = sprite.pieces[static_cast<std::size_t>(index)];
                    for (auto& coord : piece.tcoords) {
                        input >> coord.u >> coord.v;
                    }
                    piece.has_tcoords = true;
                }
            }
            continue;
        }

        if (pending_control && is_open_brace(line)) {
            in_control = true;
            pending_control = false;
            control_nested_depth = 0;
            control = UiControlDef{};
            control.id = pending_id;
            continue;
        }

        if (in_control) {
            const auto lower_line = lowercase(line);
            if (control_nested_depth == 0 && (lower_line == L"leftbutton" || lower_line == L"rightbutton")) {
                pending_control_section = lower_line;
                continue;
            }
            if (is_open_brace(line)) {
                ++control_nested_depth;
                if (control_nested_depth == 1 && !pending_control_section.empty()) {
                    active_control_section = std::move(pending_control_section);
                    pending_control_section.clear();
                }
                continue;
            }
            if (is_close_brace(line)) {
                if (control_nested_depth > 0) {
                    --control_nested_depth;
                    if (control_nested_depth == 0) {
                        active_control_section.clear();
                    }
                    continue;
                }
                if (control.id == 0) {
                    control.id = -1 - static_cast<int>(window.controls.size());
                }
                window.controls.push_back(std::move(control));
                in_control = false;
                continue;
            }
            if (control_nested_depth > 0) {
                if (control_nested_depth == 1 && !active_control_section.empty()) {
                    auto& button = active_control_section == L"leftbutton"
                        ? control.left_button
                        : control.right_button;
                    std::wistringstream input(line);
                    std::wstring key;
                    input >> key;
                    key = lowercase(std::move(key));
                    if (key == L"position") {
                        input >> button.x >> button.y;
                    } else if (key == L"size") {
                        input >> button.width >> button.height;
                    } else if (key == L"checkedimage") {
                        button.checked_image = parse_quoted(line);
                    } else if (key == L"focusedimage") {
                        button.focused_image = parse_quoted(line);
                    } else if (key == L"disabledimage") {
                        button.disabled_image = parse_quoted(line);
                    }
                }
                continue;
            }

            std::wistringstream input(line);
            std::wstring key;
            input >> key;
            key = lowercase(std::move(key));
            if (key == L"classid") {
                input >> control.class_id;
            } else if (key == L"position") {
                input >> control.x >> control.y;
            } else if (key == L"size") {
                input >> control.width >> control.height;
            } else if (key == L"font") {
                input >> control.font_index;
            } else if (key == L"textcolor") {
                control.text_color = parse_color(input, control.text_color);
            } else if (key == L"disabledcolor") {
                control.disabled_color = parse_color(input, control.disabled_color);
            } else if (key == L"focuscolor") {
                control.focus_color = parse_color(input, control.focus_color);
            } else if (key == L"windowtext") {
                input >> control.text_key;
                if (control.text_key.empty()) {
                    control.text_key = parse_quoted(line);
                }
            } else if (key == L"textformat") {
                control.text_center = line.find(L"CENTER") != std::wstring::npos;
            } else if (key == L"checkedimage") {
                control.checked_image = parse_quoted(line);
            } else if (key == L"uncheckedimage") {
                control.unchecked_image = parse_quoted(line);
            } else if (key == L"focusedimage") {
                control.focused_image = parse_quoted(line);
            } else if (key == L"image") {
                control.image_name = parse_quoted(line);
            } else if (key == L"drawmethod") {
                control.draw_sprite_name = parse_quoted(line);
            } else if (key == L"windowhelp") {
                control.window_help = parse_quoted(line);
            } else if (key == L"slotempty") {
                control.slot_empty_image = parse_quoted(line);
            } else if (key == L"slotfull") {
                control.slot_full_image = parse_quoted(line);
            } else if (key == L"slotborder") {
                control.slot_border_image = parse_quoted(line);
            } else if (key == L"scrollspr") {
                control.scroll_sprite_name = parse_quoted(line);
                const auto end_quote = line.find(L'"', line.find(L'"') + 1);
                if (end_quote != std::wstring::npos) {
                    std::wistringstream size_input(line.substr(end_quote + 1));
                    size_input >> control.scroll_sprite_width >> control.scroll_sprite_height;
                }
            } else if (key == L"deltastep") {
                input >> control.delta_step;
            } else if (key == L"password") {
                std::wstring value;
                input >> value;
                control.password = value == L"true" || value == L"TRUE" || value == L"1";
            } else if (key == L"buttonstyle") {
                control.send_quit = line.find(L"SEND_QUIT") != std::wstring::npos;
                control.send_help = line.find(L"SEND_HELP") != std::wstring::npos;
            } else if (key == L"hidden") {
                std::wstring value;
                input >> value;
                control.hidden = value == L"true" || value == L"TRUE" || value == L"1";
            } else if (key == L"disabled") {
                std::wstring value;
                input >> value;
                control.disabled = value == L"true" || value == L"TRUE" || value == L"1";
            }
            continue;
        }

        std::wistringstream input(line);
        std::wstring key;
        input >> key;
        key = lowercase(std::move(key));
        if (key == L"windowname") {
            window.name = parse_quoted(line);
        } else if (!window.name.empty() && key == L"windowtext") {
            input >> window.text_key;
            if (window.text_key.empty()) {
                window.text_key = parse_quoted(line);
            }
        } else if (!window.name.empty() && key == L"position") {
            input >> window.x >> window.y;
        } else if (!window.name.empty() && key == L"size") {
            input >> window.width >> window.height;
        } else if (!window.name.empty() && key == L"font") {
            input >> window.font_index;
        } else if (!window.name.empty() && key == L"textcolor") {
            window.text_color = parse_color(input, window.text_color);
        } else if (!window.name.empty() && key == L"recttitle") {
            input >> window.title_left >> window.title_top >> window.title_right >> window.title_bottom;
        } else if (!window.name.empty() && key == L"alignwin") {
            window.align_center_x = line.find(L"CENTER_X") != std::wstring::npos;
            window.align_center_y = line.find(L"CENTER_Y") != std::wstring::npos;
            window.align_right_x = line.find(L"RIGHT_X") != std::wstring::npos;
            window.align_right_y = line.find(L"RIGHT_Y") != std::wstring::npos;
        } else if (!window.name.empty() && key == L"savelastposition") {
            window.save_last_position = parse_bool(input, window.save_last_position);
        } else if (!window.name.empty() && key == L"candragdrop") {
            window.can_drag_drop = parse_bool(input, window.can_drag_drop);
        } else if (!window.name.empty() && key == L"cannotcross") {
            window.can_not_cross = parse_bool(input, window.can_not_cross);
        } else if (!window.name.empty() && key == L"cangotop") {
            window.can_go_top = parse_bool(input, window.can_go_top);
        } else if (!window.name.empty() && key == L"escapehandle") {
            window.escape_handle = parse_bool(input, window.escape_handle);
        } else if (!window.name.empty() && key == L"drawmethod") {
            window.draw_none = line.find(L"NONE") != std::wstring::npos;
            window.draw_sprite_name = parse_quoted(line);
        }
    }

    if (window.name.empty()) {
        throw std::runtime_error("UI file does not contain windowName: " + path.string());
    }
    return window;
}

const std::wstring& lookup_ui_string(const UiStringTable& strings, const std::wstring& key) {
    static const std::wstring empty;
    const auto it = strings.find(key);
    if (it == strings.end()) {
        return empty;
    }
    return it->second;
}

} // namespace sphere::client
