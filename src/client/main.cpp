#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <shellapi.h>

#include "client/bitmap_font.hpp"
#include "client/client_runtime.hpp"
#include "client/d3d_character_scene.hpp"
#include "client/d3d_game_world_scene.hpp"
#include "client/dds_bitmap.hpp"
#include "client/lua_runtime.hpp"
#include "client/network_client.hpp"
#include "client/ui_definition.hpp"
#include "common/binary_reader.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kBaseWidth = 1024;
constexpr int kBaseHeight = 768;
constexpr std::size_t kMaxLoginChars = 63;
constexpr std::size_t kMaxPasswordChars = 29;
constexpr std::size_t kMaxCharacterNameChars = 15;
constexpr UINT kLoginProbeMessage = WM_APP + 1;
constexpr UINT kCharacterActionMessage = WM_APP + 2;
constexpr UINT_PTR kRenderTimer = 1;

enum class AppMode {
    Login,
    CharacterSelect3D,
    Game,
};

struct TipOfDay {
    std::wstring title;
    std::wstring body;
    bool loaded = false;
};

struct ClientSettings {
    std::filesystem::path root;
    int width = kBaseWidth;
    int height = kBaseHeight;
    bool windowed = true;
    int lang = 0;
    int sound_volume = 80;
    int music_volume = 80;
    bool hardware_mix = false;
    int depth = 32;
    int grass_quality = 2;
    int shadow_quality = 0;
    bool auto_fog = true;
    int fog_distance = 200;
    int reflection_quality = 3;
    int effects_quality = 0;
    bool lods = true;
    int lod_distance = 100;
    int min_lod_distance = 100;
    bool post_effects = true;
    std::string host = "127.0.0.1";
    int port = 25860;
    int connect_type = 1;
    int scan_forward = 17;
    int scan_backward = 31;
    int scan_strafe_left = 30;
    int scan_strafe_right = 32;
    int key_cursor = VK_TAB;
    int key_run = 'R';
    bool always_run = true;
    std::string registration_url;
    bool debug_auto_enter = false;
    bool debug_start_character_scene = false;
    bool debug_start_game_world = false;
    double debug_game_x = 0.0;
    double debug_game_y = 0.0;
    double debug_game_z = 0.0;
    double debug_game_angle = 0.0;
};

struct CharacterAppearance {
    int gender = 0;
    int face = 0;
    int hair = 0;
    int hair_color = 0;
    int tattoo = 0;
    int strength = 10;
    int dexterity = 10;
    int accuracy = 10;
    int endurance = 10;
    int fire = 0;
    int water = 0;
    int earth = 0;
    int air = 0;
};

struct GameWorldState {
    bool has_spawn = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double angle = 0.0;
    int selected_slot = 0;
    int select_packet_count = 0;
    int select_byte_count = 0;
    int world_packet_count = 0;
    int world_byte_count = 0;
    std::wstring character_name;
};

enum class UiDragKind {
    None,
    CharacterSelect,
    Game,
};

struct AppState {
    ClientSettings settings;
    sphere::client::ClientRuntime runtime;
    std::unique_ptr<sphere::client::LuaRuntime> lua_runtime;
    sphere::client::LuaBootResult lua_boot;
    sphere::client::UiWindowDef pick_person_window;
    std::vector<sphere::client::UiWindowDef> game_windows;
    std::vector<bool> game_window_visible;
    std::vector<int> game_window_z_order;
    std::unordered_map<std::uint64_t, bool> game_control_checked;
    int settings_window_start_index = -1;
    sphere::client::BitmapImage background;
    std::unordered_map<std::wstring, sphere::client::BitmapImage> textures;
    std::unordered_map<int, sphere::client::BitmapFont> fonts;
    RECT background_rect{};
    RECT panel_rect{};
    HFONT ui_font = nullptr;
    std::unique_ptr<sphere::client::CharacterSelectScene> character_scene;
    std::unique_ptr<sphere::client::GameWorldScene> game_scene;
    AppMode mode = AppMode::Login;
    TipOfDay tip;
    std::wstring login;
    std::wstring password;
    bool save_password = true;
    int active_edit_id = 7;
    int hot_control_id = 0;
    int pressed_control_id = 0;
    int character_hot_control_id = 0;
    int character_pressed_control_id = 0;
    int game_hot_control_id = 0;
    int game_pressed_control_id = 0;
    int game_hot_window_index = -1;
    int game_pressed_window_index = -1;
    int game_active_edit_window_index = -1;
    int game_active_edit_control_id = 0;
    std::wstring game_edit_text;
    std::vector<std::wstring> game_chat_lines;
    std::wstring game_help_text;
    POINT game_pressed_mouse{};
    std::optional<ClientSettings> settings_dialog_backup;
    UiDragKind ui_drag_kind = UiDragKind::None;
    int ui_drag_game_window_index = -1;
    POINT ui_drag_last_mouse{};
    bool character_overlay_dirty = false;
    bool game_overlay_dirty = false;
    int character_camera_focus_id = 0;
    bool rotating_character = false;
    POINT last_character_mouse{};
    int character_spin_delta = 1;
    CharacterAppearance appearance;
    std::array<sphere::client::CharacterSlot, 3> character_slots{};
    std::array<std::wstring, 3> character_name_edits{};
    std::shared_ptr<sphere::client::ServerSession> server_session;
    int selected_character_slot = 0;
    int active_character_edit_id = 60;
    bool character_action_in_progress = false;
    bool character_entered_game = false;
    bool login_in_progress = false;
    sphere::client::GameMovementInput game_movement;
    bool game_look_mode = false;
    POINT game_mouse_center{};
    ULONGLONG game_last_tick = 0;
    ULONGLONG game_last_position_send_tick = 0;
    ULONGLONG game_last_overlay_tick = 0;
    bool has_game_time = false;
    float game_time_fraction = 0.0f;
    GameWorldState game_world;
    std::wstring status;
};

enum class CharacterActionKind {
    Select,
    Create,
    Delete,
    Ack,
};

struct PostedCharacterActionResult {
    CharacterActionKind kind = CharacterActionKind::Select;
    int slot = 0;
    std::wstring name;
    sphere::client::CharacterCreationAppearance appearance;
    sphere::client::CharacterActionResult result;
    sphere::client::CharacterActionResult ack_result;
};

std::unique_ptr<AppState> g_app;

bool update_game_overlay(HWND hwnd);

void append_client_log(const std::wstring& message) {
    if (!g_app) {
        return;
    }
    std::wofstream log(g_app->settings.root / "newclient.log", std::ios::app);
    if (log) {
        log << message << L"\n";
    }
}

struct SoundDllState {
    using CreateInterfaceFn = void* (__cdecl*)(HWND, int, unsigned long, unsigned long);
    using CloseFn = void (__cdecl*)();
    using SetStreamVolumeFn = void (__cdecl*)(int);
    using StreamCreateFileFn = unsigned long (__cdecl*)(const char*, unsigned long);
    using StreamPlayFn = unsigned long (__cdecl*)(unsigned long, unsigned long);
    using StreamStopFn = void (__cdecl*)(unsigned long);
    using StreamFreeFn = void (__cdecl*)(unsigned long);

    HMODULE module = nullptr;
    unsigned long stream = 0;
    CreateInterfaceFn create_interface = nullptr;
    CloseFn close = nullptr;
    SetStreamVolumeFn set_stream_volume = nullptr;
    StreamCreateFileFn stream_create_file = nullptr;
    StreamPlayFn stream_play = nullptr;
    StreamStopFn stream_stop = nullptr;
    StreamFreeFn stream_free = nullptr;
};

SoundDllState g_sound;

std::wstring widen_ascii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(::towlower(ch));
    });
    return value;
}

bool class_is(const sphere::client::UiControlDef& control, const wchar_t* class_id) {
    return lowercase(control.class_id) == lowercase(class_id);
}

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

std::wstring parse_quoted_value(const std::wstring& line) {
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

bool starts_with_case_insensitive(const std::wstring& text, std::size_t offset, const wchar_t* prefix) {
    for (std::size_t i = 0; prefix[i] != 0; ++i) {
        if (offset + i >= text.size() || std::towlower(text[offset + i]) != std::towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::wstring strip_hypertext_markup(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (starts_with_case_insensitive(text, i, L"<BR>")) {
            out.push_back(L'\n');
            i += 4;
            continue;
        }
        if (text[i] == L'<') {
            const auto end = text.find(L'>', i + 1);
            if (end != std::wstring::npos) {
                i = end + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return trim(std::move(out));
}

std::filesystem::path executable_directory() {
    wchar_t path[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

std::string narrow_ansi(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, "?", nullptr);
    std::string out(static_cast<std::size_t>((std::max)(needed, 0)), '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed, "?", nullptr);
    }
    return out;
}

std::filesystem::path saved_login_path(const std::filesystem::path& root) {
    return root / "login";
}

void load_saved_login(AppState& state) {
    const auto path = saved_login_path(state.settings.root);
    if (!std::filesystem::exists(path)) {
        return;
    }
    try {
        const auto text = sphere::client::decode_cp1251_file(path);
        std::wistringstream lines(text);
        std::wstring login;
        std::wstring password;
        std::getline(lines, login);
        std::getline(lines, password);
        state.login = trim(std::move(login));
        state.password = trim(std::move(password));
        if (!state.login.empty() || !state.password.empty()) {
            state.save_password = true;
        }
    } catch (...) {
    }
}

void save_login_file() {
    if (!g_app) {
        return;
    }

    const auto path = saved_login_path(g_app->settings.root);
    if (!g_app->save_password) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return;
    }

    try {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << narrow_ansi(g_app->login) << "\r\n";
        out << narrow_ansi(g_app->password) << "\r\n";
    } catch (...) {
    }
}

std::wstring tip_file_name_for_lang(int lang) {
    switch (lang) {
    case 1:
        return L"tipoftheday_e.txt";
    case 2:
        return L"tipoftheday_i.txt";
    case 3:
        return L"tipoftheday_p.txt";
    default:
        return L"tipoftheday.txt";
    }
}

bool looks_like_plain_message_group(const std::wstring& text) {
    return text.find(L"<cl=") != std::wstring::npos && text.rfind(L"SPHR", 0) != 0;
}

std::optional<std::wstring> try_load_plain_message_group(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
    try {
        auto text = sphere::client::decode_cp1251_file(path);
        if (looks_like_plain_message_group(text)) {
            return text;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::wstring> load_decoded_tip_group_text(const std::filesystem::path& root, int lang) {
    const auto file_name = tip_file_name_for_lang(lang);
    const std::filesystem::path candidates[] = {
        root / "language" / file_name,
        root.parent_path() / "sphDecoded" / "language" / file_name,
    };
    for (const auto& candidate : candidates) {
        if (auto text = try_load_plain_message_group(candidate)) {
            return text;
        }
    }
    return std::nullopt;
}

std::unordered_map<int, std::wstring> parse_message_group(const std::wstring& text) {
    std::unordered_map<int, std::wstring> messages;
    std::wistringstream lines(text);
    std::wstring line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        std::wistringstream input(line);
        std::wstring id_text;
        input >> id_text;
        if (id_text.empty() || !std::iswdigit(id_text.front())) {
            continue;
        }

        int id = 0;
        try {
            id = std::stoi(id_text);
        } catch (...) {
            continue;
        }

        std::wstring rest;
        std::getline(input, rest);
        messages[id] = trim(std::move(rest));
    }
    return messages;
}

int tip_count(const std::unordered_map<int, std::wstring>& messages) {
    int count = 0;
    while (messages.find(count) != messages.end()) {
        ++count;
    }
    return count;
}

int load_tip_index(const std::filesystem::path& root, int count) {
    int last = 0;
    int number = count;
    try {
        const auto cfg = sphere::config::ConfigFile::load(root / "players" / "totdparams.cfg");
        last = cfg.get_int("LAST", last);
        number = cfg.get_int("NUMBER", number);
    } catch (...) {
    }
    if (count <= 0) {
        return 0;
    }
    if (number != count && number >= 0 && number < count) {
        return number;
    }
    return std::clamp(last, 0, count - 1);
}

TipOfDay load_tip_of_day(const std::filesystem::path& root, int lang) {
    TipOfDay tip;
    auto text = load_decoded_tip_group_text(root, lang);
    if (!text) {
        return tip;
    }

    const auto messages = parse_message_group(*text);
    const int count = tip_count(messages);
    const int index = load_tip_index(root, count);
    const auto title_it = messages.find(1024);
    const auto body_it = messages.find(index);
    if (title_it == messages.end() || body_it == messages.end()) {
        return tip;
    }

    tip.title = strip_hypertext_markup(title_it->second);
    tip.body = strip_hypertext_markup(body_it->second);
    tip.loaded = !tip.title.empty() && !tip.body.empty();
    return tip;
}

std::filesystem::path parse_root_from_command_line() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return executable_directory();
    }

    std::filesystem::path root = executable_directory();
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--root" && i + 1 < argc) {
            root = argv[++i];
        }
    }
    LocalFree(argv);
    return root;
}

bool command_line_has_switch(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == name) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

ClientSettings load_settings() {
    ClientSettings settings;
    settings.root = parse_root_from_command_line();
    settings.debug_auto_enter = command_line_has_switch(L"--debug-auto-enter");
    settings.debug_start_character_scene = command_line_has_switch(L"--debug-character-select-3d");
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i + 4 < argc; ++i) {
            if (std::wstring(argv[i]) == L"--debug-game-world") {
                settings.debug_start_game_world = true;
                settings.debug_game_x = std::wcstod(argv[i + 1], nullptr);
                settings.debug_game_y = std::wcstod(argv[i + 2], nullptr);
                settings.debug_game_z = std::wcstod(argv[i + 3], nullptr);
                settings.debug_game_angle = std::wcstod(argv[i + 4], nullptr);
                break;
            }
        }
        LocalFree(argv);
    }

    const auto config = sphere::config::ConfigFile::load(settings.root / "config.cfg");
    const auto connect = sphere::config::ConfigFile::load(settings.root / "connect.cfg");
    const auto control = sphere::config::ConfigFile::load(settings.root / "control.cfg");

    settings.width = config.get_int("XRES", settings.width);
    settings.height = config.get_int("YRES", settings.height);
    settings.windowed = config.get_int("WINDOWED", settings.windowed ? 1 : 0) != 0;
    settings.lang = config.get_int("LANG", settings.lang);
    settings.sound_volume = std::clamp(config.get_int("SNDVOL", settings.sound_volume), 0, 100);
    settings.music_volume = std::clamp(config.get_int("MUSVOL", settings.music_volume), 0, 100);
    settings.hardware_mix = config.get_int("HWMIX", settings.hardware_mix ? 1 : 0) != 0;
    settings.depth = config.get_int("DEPTH", settings.depth);
    settings.grass_quality = std::clamp(config.get_int("GRASS", settings.grass_quality), 0, 2);
    settings.shadow_quality = std::clamp(config.get_int("SHAD", settings.shadow_quality), 0, 4);
    settings.auto_fog = config.get_int("AUTOFOG", settings.auto_fog ? 1 : 0) != 0;
    settings.fog_distance = std::clamp(config.get_int("FOGDIST", settings.fog_distance), 30, 200);
    settings.reflection_quality = std::clamp(config.get_int("REFLQUAL", settings.reflection_quality), 0, 3);
    settings.effects_quality = std::clamp(config.get_int("EFFECTS", settings.effects_quality), 0, 1);
    settings.lods = config.get_int("LODS", settings.lods ? 1 : 0) != 0;
    settings.lod_distance = config.get_int("LOD_DISTANCE", settings.lod_distance);
    settings.min_lod_distance = config.get_int("MIN_LOD_DIST", settings.min_lod_distance);
    settings.post_effects = config.get_int("POSTEFFECTS", settings.post_effects ? 1 : 0) != 0;
    settings.host = connect.get_string("MAIN_URL", settings.host);
    settings.port = connect.get_int("PORT", settings.port);
    settings.connect_type = connect.get_int("CONNECT_TYPE", settings.connect_type);
    settings.scan_forward = control.get_int("CodeFWD", settings.scan_forward);
    settings.scan_backward = control.get_int("CodeBACK", settings.scan_backward);
    settings.scan_strafe_left = control.get_int("CodeSLEFT", settings.scan_strafe_left);
    settings.scan_strafe_right = control.get_int("CodeSRIGHT", settings.scan_strafe_right);
    settings.key_cursor = control.get_int("KeyCURS", settings.key_cursor);
    settings.key_run = control.get_int("KeyRUN", settings.key_run);
    settings.always_run = control.get_int("ALWRUN", settings.always_run ? 1 : 0) != 0;

    try {
        const auto connectn = sphere::config::ConfigFile::load(settings.root / "connectn.cfg");
        settings.registration_url = connectn.get_string("REG_SRV", "");
    } catch (...) {
        settings.registration_url.clear();
    }
    return settings;
}

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

void write_config_values(const std::filesystem::path& path, const std::unordered_map<std::string, int>& values) {
    std::vector<std::string> lines;
    {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }

    std::unordered_map<std::string, bool> written;
    for (const auto& [key, value] : values) {
        written.emplace(uppercase_ascii(key), false);
    }

    for (auto& line : lines) {
        std::istringstream input(line);
        std::string key;
        input >> key;
        const auto normalized = uppercase_ascii(key);
        const auto value = values.find(normalized);
        if (value == values.end()) {
            continue;
        }
        line = normalized + "\t" + std::to_string(value->second);
        written[normalized] = true;
    }

    for (const auto& [key, value] : values) {
        const auto normalized = uppercase_ascii(key);
        if (!written[normalized]) {
            lines.push_back(normalized + "\t" + std::to_string(value));
        }
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write config: " + path.string());
    }
    for (const auto& line : lines) {
        output << line << '\n';
    }
}

void save_graphics_settings() {
    const auto& settings = g_app->settings;
    write_config_values(settings.root / "config.cfg", {
        {"XRES", settings.width},
        {"YRES", settings.height},
        {"DEPTH", settings.depth},
        {"GRASS", settings.grass_quality},
        {"WINDOWED", settings.windowed ? 1 : 0},
        {"SHAD", settings.shadow_quality},
        {"AUTOFOG", settings.auto_fog ? 1 : 0},
        {"FOGDIST", settings.fog_distance},
        {"REFLQUAL", settings.reflection_quality},
        {"EFFECTS", settings.effects_quality},
        {"LODS", settings.lods ? 1 : 0},
        {"LOD_DISTANCE", settings.lod_distance},
        {"MIN_LOD_DIST", settings.min_lod_distance},
        {"POSTEFFECTS", settings.post_effects ? 1 : 0},
    });
}

void save_sound_settings() {
    const auto& settings = g_app->settings;
    write_config_values(settings.root / "config.cfg", {
        {"SNDVOL", settings.sound_volume},
        {"MUSVOL", settings.music_volume},
        {"HWMIX", settings.hardware_mix ? 1 : 0},
    });
}

std::wstring make_title(const ClientSettings& settings) {
    std::ostringstream title;
    title << "Sphere NewClient - " << settings.host << ":" << settings.port
          << " ct=" << settings.connect_type;
    return widen_ascii(title.str());
}

std::filesystem::path scene_music_path(const std::filesystem::path& root) {
    const auto soundtrack = root / "sounds" / "music" / "start.sst";
    try {
        const auto text = sphere::client::decode_cp1251_file(soundtrack);
        std::wistringstream lines(text);
        std::wstring line;
        while (std::getline(lines, line)) {
            line = trim(std::move(line));
            if (line.rfind(L"audio_file", 0) == 0) {
                const auto quoted = parse_quoted_value(line);
                if (!quoted.empty()) {
                    return root / std::filesystem::path(quoted);
                }
            }
        }
    } catch (...) {
    }
    return root / "sounds" / "music" / "mounts_1d.ogg";
}

void stop_scene_music() {
    if (g_sound.stream != 0) {
        if (g_sound.stream_stop) {
            g_sound.stream_stop(g_sound.stream);
        }
        if (g_sound.stream_free) {
            g_sound.stream_free(g_sound.stream);
        }
        g_sound.stream = 0;
    }
    if (g_sound.close) {
        g_sound.close();
    }
    if (g_sound.module) {
        FreeLibrary(g_sound.module);
        g_sound = SoundDllState{};
    }
    mciSendStringW(L"stop sphere_newclient_music", nullptr, 0, nullptr);
    mciSendStringW(L"close sphere_newclient_music", nullptr, 0, nullptr);
}

template <typename T>
bool load_sound_proc(T& target, const char* name) {
    target = reinterpret_cast<T>(GetProcAddress(g_sound.module, name));
    return target != nullptr;
}

bool load_sound_dll(const std::filesystem::path& root, HWND hwnd) {
    if (g_sound.module) {
        return true;
    }

    const auto dll_path = root / "sound.dll";
    if (!std::filesystem::exists(dll_path)) {
        return false;
    }

    g_sound.module = LoadLibraryW(dll_path.c_str());
    if (!g_sound.module) {
        return false;
    }
    if (!load_sound_proc(g_sound.create_interface, "?SI_CreateInterface@@YAPAVCSoundInterface@@PAUHWND__@@HKK@Z") ||
        !load_sound_proc(g_sound.close, "?SI_Close@@YAXXZ") ||
        !load_sound_proc(g_sound.set_stream_volume, "?SI_SetStreamVolume@@YAXH@Z") ||
        !load_sound_proc(g_sound.stream_create_file, "?SI_StreamCreateFile@@YAKPBDK@Z") ||
        !load_sound_proc(g_sound.stream_play, "?SI_StreamPlay@@YAKKK@Z") ||
        !load_sound_proc(g_sound.stream_stop, "?SI_StreamStop@@YAXK@Z") ||
        !load_sound_proc(g_sound.stream_free, "?SI_StreamFree@@YAXK@Z")) {
        stop_scene_music();
        return false;
    }
    if (!g_sound.create_interface(hwnd, -1, 0xac44, 0)) {
        stop_scene_music();
        return false;
    }
    return true;
}

bool start_scene_music_with_sound_dll(HWND hwnd, const std::filesystem::path& path) {
    if (!load_sound_dll(g_app->settings.root, hwnd)) {
        return false;
    }

    g_sound.set_stream_volume(std::clamp(g_app->settings.music_volume, 0, 100));
    const auto narrow = narrow_ansi(path.wstring());
    g_sound.stream = g_sound.stream_create_file(narrow.c_str(), 1);
    if (g_sound.stream == 0) {
        stop_scene_music();
        return false;
    }
    if (g_sound.stream_play(g_sound.stream, 1) == 0) {
        stop_scene_music();
        return false;
    }
    return true;
}

void start_scene_music_with_mci(const std::filesystem::path& path) {
    std::wstring command = L"open \"";
    command += path.wstring();
    command += L"\" alias sphere_newclient_music";
    if (mciSendStringW(command.c_str(), nullptr, 0, nullptr) != 0) {
        return;
    }

    std::wostringstream volume_command;
    volume_command << L"setaudio sphere_newclient_music volume to " << std::clamp(g_app->settings.music_volume * 10, 0, 1000);
    mciSendStringW(volume_command.str().c_str(), nullptr, 0, nullptr);
    mciSendStringW(L"play sphere_newclient_music repeat", nullptr, 0, nullptr);
}

void start_scene_music(HWND hwnd) {
    stop_scene_music();
    if (!g_app || g_app->settings.music_volume <= 0) {
        return;
    }
    const auto path = scene_music_path(g_app->settings.root);
    if (!std::filesystem::exists(path)) {
        return;
    }

    if (start_scene_music_with_sound_dll(hwnd, path)) {
        return;
    }
    start_scene_music_with_mci(path);
}

void apply_music_volume() {
    const int volume = std::clamp(g_app->settings.music_volume, 0, 100);
    if (g_sound.set_stream_volume) {
        g_sound.set_stream_volume(volume);
    }
    std::wostringstream command;
    command << L"setaudio sphere_newclient_music volume to " << volume * 10;
    mciSendStringW(command.str().c_str(), nullptr, 0, nullptr);
}

void apply_grass_quality() {
    if (!g_app->game_scene) {
        return;
    }
    std::wstring error;
    if (!g_app->game_scene->set_grass_quality(g_app->settings.grass_quality, error)) {
        append_client_log(error);
    }
}

std::wstring resolve_text(const sphere::client::UiControlDef& def) {
    if (def.text_key.empty()) {
        return {};
    }
    const auto& value = sphere::client::lookup_ui_string(g_app->runtime.strings, def.text_key);
    return value.empty() && lowercase(def.text_key).starts_with(L"uistr_") ? L"" : (value.empty() ? def.text_key : value);
}

std::wstring ui_string(const wchar_t* key) {
    const auto& value = sphere::client::lookup_ui_string(g_app->runtime.strings, key);
    return value.empty() ? std::wstring(key) : value;
}

std::wstring connection_title_text() {
    // The migrated _main/_gmsg scripts set the window title through gMsg(956).
    // The encrypted _sys message container is not implemented yet, so keep
    // this single login-window system text mapped until the Lua gMsg path is wired.
    switch (g_app->settings.lang) {
    case 1:
        return L"Connection";
    case 2:
        return L"Connessione";
    case 3:
        return L"Conexao";
    default:
        return L"\x041F\x043E\x0434\x043A\x043B\x044E\x0447\x0435\x043D\x0438\x0435";
    }
}

void apply_login_script_bootstrap(sphere::client::UiWindowDef& window) {
    // Start_Win in the migrated _main script hides the registration button:
    // window_api(25, window_api(24, g_80FF4, 11), 108, 1, 0, 0)
    for (auto& control : window.controls) {
        if (control.id == 11) {
            control.hidden = true;
        }
    }
}

COLORREF to_color_ref(const sphere::client::UiColor& color) {
    return RGB(
        std::clamp(color.r, 0, 255),
        std::clamp(color.g, 0, 255),
        std::clamp(color.b, 0, 255));
}

void try_load_font(std::unordered_map<int, sphere::client::BitmapFont>& fonts, const std::filesystem::path& root, int index, const std::wstring& name) {
    if (name.empty()) {
        return;
    }
    try {
        fonts.emplace(index, sphere::client::BitmapFont::load(root, name));
    } catch (...) {
    }
}

std::unordered_map<int, sphere::client::BitmapFont> load_fonts(const std::filesystem::path& root) {
    std::unordered_map<int, sphere::client::BitmapFont> fonts;
    try {
        const auto cfg = sphere::config::ConfigFile::load(root / "fonts.cfg");
        const int count = cfg.get_int("NEW_FONTS_NUMBER", 0);
        for (int i = 0; i < count; ++i) {
            const auto font_name = cfg.get_string("NEW_FONT_" + std::to_string(i), "");
            try_load_font(fonts, root, i, widen_ascii(font_name));
        }
    } catch (...) {
    }
    if (fonts.empty()) {
        try_load_font(fonts, root, 0, L"fnt_sphere0");
    }
    return fonts;
}

const sphere::client::BitmapFont* font_for(int index) {
    const int mapped_index = index >= 2 ? index - 2 : index;
    auto it = g_app->fonts.find(mapped_index);
    if (it != g_app->fonts.end() && it->second.valid()) {
        return &it->second;
    }
    it = g_app->fonts.find(0);
    if (it != g_app->fonts.end() && it->second.valid()) {
        return &it->second;
    }
    for (const auto& [_, font] : g_app->fonts) {
        if (font.valid()) {
            return &font;
        }
    }
    return nullptr;
}

RECT background_rect_for(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int client_w = rc.right - rc.left;
    const int client_h = rc.bottom - rc.top;
    const int x = (client_w - kBaseWidth) / 2;
    const int y = (client_h - kBaseHeight) / 2;
    return RECT{x, y, x + kBaseWidth, y + kBaseHeight};
}

void update_layout(HWND hwnd) {
    if (!g_app) {
        return;
    }
    g_app->background_rect = background_rect_for(hwnd);
    const int panel_w = g_app->runtime.connection_window.width;
    const int panel_h = g_app->runtime.connection_window.height;
    const int panel_x = g_app->background_rect.left + (kBaseWidth - panel_w) / 2;
    const int panel_y = g_app->background_rect.top + (kBaseHeight - panel_h) / 2;
    g_app->panel_rect = RECT{panel_x, panel_y, panel_x + panel_w, panel_y + panel_h};
}

RECT control_rect(const sphere::client::UiControlDef& def) {
    return RECT{
        g_app->panel_rect.left + def.x,
        g_app->panel_rect.top + def.y,
        g_app->panel_rect.left + def.x + def.width,
        g_app->panel_rect.top + def.y + def.height,
    };
}

RECT control_rect_at(RECT window_rect, const sphere::client::UiControlDef& def) {
    return RECT{
        window_rect.left + def.x,
        window_rect.top + def.y,
        window_rect.left + def.x + def.width,
        window_rect.top + def.y + def.height,
    };
}

const sphere::client::UiControlDef* control_by_id(int id) {
    for (const auto& control : g_app->runtime.connection_window.controls) {
        if (!control.hidden && !control.disabled && control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

bool is_clickable_control(const sphere::client::UiControlDef& control) {
    return class_is(control, L"Button") || class_is(control, L"CheckBox") || class_is(control, L"Edit");
}

bool is_hot_control(const sphere::client::UiControlDef& control) {
    return class_is(control, L"Button") || class_is(control, L"CheckBox");
}

int hit_test(POINT pt, bool hot_only) {
    for (const auto& control : g_app->runtime.connection_window.controls) {
        if (control.hidden || control.disabled) {
            continue;
        }
        if (hot_only ? !is_hot_control(control) : !is_clickable_control(control)) {
            continue;
        }
        RECT rc = control_rect(control);
        if (PtInRect(&rc, pt)) {
            return control.id;
        }
    }
    return 0;
}

void invalidate_control(HWND hwnd, int id) {
    if (const auto* control = control_by_id(id)) {
        RECT rc = control_rect(*control);
        InflateRect(&rc, 8, 8);
        InvalidateRect(hwnd, &rc, FALSE);
    }
}

void invalidate_controls(HWND hwnd, int first_id, int second_id = 0) {
    if (first_id != 0) {
        invalidate_control(hwnd, first_id);
    }
    if (second_id != 0 && second_id != first_id) {
        invalidate_control(hwnd, second_id);
    }
}

std::filesystem::path texture_path(const std::wstring& texture_name) {
    const auto name = lowercase(texture_name);
    const std::wstring file = name + L".dds";
    const std::filesystem::path root = g_app->settings.root;
    const std::filesystem::path candidates[] = {
        root / "textures" / "fx" / file,
        root / "xadd" / file,
        root / "textures" / file,
        root / "models" / "textures" / file,
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return candidates[0];
}

sphere::client::BitmapImage* texture_for(const std::wstring& texture_name) {
    const auto key = lowercase(texture_name);
    auto it = g_app->textures.find(key);
    if (it != g_app->textures.end()) {
        return &it->second;
    }

    try {
        auto loaded = sphere::client::load_dds_rgb_bitmap(texture_path(texture_name));
        auto [inserted, _] = g_app->textures.emplace(key, std::move(loaded));
        return &inserted->second;
    } catch (...) {
        return nullptr;
    }
}

void preload_window_textures(const sphere::client::UiWindowDef& window) {
    for (const auto& [_, sprite] : window.sprites) {
        for (const auto& piece : sprite.pieces) {
            texture_for(piece.texture_name);
        }
    }
}

void preload_connection_textures() {
    preload_window_textures(g_app->runtime.connection_window);
    preload_window_textures(g_app->pick_person_window);
    for (const auto& window : g_app->game_windows) {
        preload_window_textures(window);
    }
}

std::vector<sphere::client::UiWindowDef> load_ui_windows(
    const std::filesystem::path& root,
    const std::vector<std::wstring>& file_names) {
    std::vector<sphere::client::UiWindowDef> windows;
    windows.reserve(file_names.size());
    for (const auto& file_name : file_names) {
        const auto path = root / "effects" / file_name;
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("game UI file is missing: " + path.string());
        }
        windows.push_back(sphere::client::load_ui_window(path));
    }
    return windows;
}

void alpha_blit(HDC dc, const sphere::client::BitmapImage& image, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    HDC mem_dc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem_dc, image.handle);
    if (image.has_alpha) {
        if (sw < 0 || sh < 0) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = dw;
            info.bmiHeader.biHeight = -dh;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HBITMAP mirrored = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            HDC tmp_dc = CreateCompatibleDC(dc);
            HGDIOBJ old_tmp = SelectObject(tmp_dc, mirrored);
            StretchBlt(tmp_dc, 0, 0, dw, dh, mem_dc, sx, sy, sw, sh, SRCCOPY);
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dc, dx, dy, dw, dh, tmp_dc, 0, 0, dw, dh, blend);
            SelectObject(tmp_dc, old_tmp);
            DeleteDC(tmp_dc);
            DeleteObject(mirrored);
            SelectObject(mem_dc, old);
            DeleteDC(mem_dc);
            return;
        }
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(dc, dx, dy, dw, dh, mem_dc, sx, sy, sw, sh, blend);
    } else {
        StretchBlt(dc, dx, dy, dw, dh, mem_dc, sx, sy, sw, sh, SRCCOPY);
    }
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
}

void textured_quad_blit(HDC dc, const sphere::client::BitmapImage& image, const sphere::client::UiSpritePiece& piece, int dx, int dy, int dw, int dh) {
    if (!image.pixels || dw <= 0 || dh <= 0) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = dw;
    info.bmiHeader.biHeight = -dh;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return;
    }

    auto sample = [&](float tx, float ty, std::uint8_t* dst) {
        const auto& p0 = piece.tcoords[0];
        const auto& p1 = piece.tcoords[1];
        const auto& p2 = piece.tcoords[2];
        const auto& p3 = piece.tcoords[3];
        const float top_u = static_cast<float>(p0.u) + (static_cast<float>(p1.u - p0.u) * tx);
        const float top_v = static_cast<float>(p0.v) + (static_cast<float>(p1.v - p0.v) * tx);
        const float bottom_u = static_cast<float>(p3.u) + (static_cast<float>(p2.u - p3.u) * tx);
        const float bottom_v = static_cast<float>(p3.v) + (static_cast<float>(p2.v - p3.v) * tx);
        const int sx = std::clamp(static_cast<int>(std::lround(top_u + (bottom_u - top_u) * ty)), 0, image.width - 1);
        const int sy = std::clamp(static_cast<int>(std::lround(top_v + (bottom_v - top_v) * ty)), 0, image.height - 1);
        const auto* src = image.pixels + static_cast<std::size_t>(sy) * image.stride + sx * 4;
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
    };

    auto* out = static_cast<std::uint8_t*>(pixels);
    const int stride = dw * 4;
    for (int y = 0; y < dh; ++y) {
        const float ty = dh <= 1 ? 0.0f : static_cast<float>(y) / static_cast<float>(dh - 1);
        auto* row = out + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < dw; ++x) {
            const float tx = dw <= 1 ? 0.0f : static_cast<float>(x) / static_cast<float>(dw - 1);
            sample(tx, ty, row + x * 4);
        }
    }

    HDC mem_dc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem_dc, bitmap);
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(dc, dx, dy, dw, dh, mem_dc, 0, 0, dw, dh, blend);
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    DeleteObject(bitmap);
}

void draw_sprite_from_window(HDC dc, const sphere::client::UiWindowDef& window, const std::wstring& sprite_name, int x, int y) {
    const auto it = window.sprites.find(lowercase(sprite_name));
    if (it == window.sprites.end()) {
        return;
    }

    for (const auto& piece : it->second.pieces) {
        auto* texture = texture_for(piece.texture_name);
        if (!texture || !*texture) {
            continue;
        }
        int sx = piece.src_left;
        int sy = piece.src_top;
        int sw = piece.src_right - piece.src_left;
        int sh = piece.src_bottom - piece.src_top;
        const int dx = x + min(piece.dst_left, piece.dst_right);
        const int dy = y + min(piece.dst_top, piece.dst_bottom);
        const int dw = abs(piece.dst_right - piece.dst_left);
        const int dh = abs(piece.dst_bottom - piece.dst_top);
        if (piece.has_tcoords) {
            textured_quad_blit(dc, *texture, piece, dx, dy, dw, dh);
            continue;
        }
        if (piece.dst_right < piece.dst_left) {
            sx += sw;
            sw = -sw;
        }
        if (piece.dst_bottom < piece.dst_top) {
            sy += sh;
            sh = -sh;
        }
        if (sw != 0 && sh != 0 && dw > 0 && dh > 0) {
            alpha_blit(dc, *texture, dx, dy, dw, dh, sx, sy, sw, sh);
        }
    }
}

void draw_sprite(HDC dc, const std::wstring& sprite_name, int x, int y) {
    draw_sprite_from_window(dc, g_app->runtime.connection_window, sprite_name, x, y);
}

std::wstring button_sprite_for(const sphere::client::UiControlDef& def) {
    const int pressed_id = g_app->mode == AppMode::CharacterSelect3D ? g_app->character_pressed_control_id : g_app->pressed_control_id;
    const int hot_id = g_app->mode == AppMode::CharacterSelect3D ? g_app->character_hot_control_id : g_app->hot_control_id;
    const bool pressed = pressed_id == def.id;
    const bool hot = hot_id == def.id;
    if (!def.unchecked_image.empty()) {
        if (pressed && !def.checked_image.empty()) {
            return def.checked_image;
        }
        if (hot && !def.focused_image.empty()) {
            return def.focused_image;
        }
        return def.unchecked_image;
    }
    if (def.id == 1) {
        return pressed ? L"close_push" : (hot ? L"close_focus" : L"close_normal");
    }
    return {};
}

void draw_ui_text(HDC dc, const std::wstring& text, RECT rc, UINT format, int font_index, const sphere::client::UiColor& color) {
    if (text.empty()) {
        return;
    }
    if (const auto* font = font_for(font_index)) {
        font->draw_text(dc, text, rc, format, to_color_ref(color), static_cast<BYTE>(std::clamp(color.a, 0, 255)));
        return;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, to_color_ref(color));
    DrawTextW(dc, text.c_str(), -1, &rc, format);
}

void draw_text_centered(HDC dc, const std::wstring& text, RECT rc, int font_index, const sphere::client::UiColor& color) {
    draw_ui_text(dc, text, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, font_index, color);
}

void draw_custom_button(HDC dc, const sphere::client::UiControlDef& def, RECT rc) {
    const int hot_id = g_app->mode == AppMode::CharacterSelect3D ? g_app->character_hot_control_id : g_app->hot_control_id;
    const auto& color = hot_id == def.id ? def.focus_color : def.text_color;
    draw_text_centered(dc, resolve_text(def), rc, def.font_index, color);
}

void draw_control(HDC dc, const sphere::client::UiControlDef& def) {
    if (def.hidden) {
        return;
    }
    RECT rc = control_rect(def);

    if (class_is(def, L"Text")) {
        const UINT align = def.text_center ? DT_CENTER : DT_LEFT;
        draw_ui_text(dc, resolve_text(def), rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.disabled ? def.disabled_color : def.text_color);
        return;
    }

    if (class_is(def, L"Edit")) {
        const bool active = g_app->active_edit_id == def.id;
        std::wstring text = def.id == 8 ? std::wstring(g_app->password.size(), L'*') : g_app->login;
        if (active) {
            text += L"_";
        }
        const UINT align = def.text_center ? DT_CENTER : DT_LEFT;
        draw_ui_text(dc, text, rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.disabled ? def.disabled_color : def.text_color);
        return;
    }

    if (class_is(def, L"Image")) {
        if (!def.image_name.empty()) {
            draw_sprite(dc, def.image_name, rc.left, rc.top);
        }
        return;
    }

    if (class_is(def, L"CheckBox")) {
        draw_sprite(dc, g_app->save_password ? L"chk_push" : (g_app->hot_control_id == def.id ? L"chk_focus" : L"chk_normal"), rc.left, rc.top);
        return;
    }

    if (class_is(def, L"Button")) {
        const auto sprite = button_sprite_for(def);
        if (!sprite.empty()) {
            draw_sprite(dc, sprite, rc.left, rc.top);
        } else {
            draw_custom_button(dc, def, rc);
            return;
        }
        if (def.id != 1) {
            const int hot_id = g_app->mode == AppMode::CharacterSelect3D ? g_app->character_hot_control_id : g_app->hot_control_id;
            const auto& color = hot_id == def.id ? def.focus_color : def.text_color;
            draw_text_centered(dc, resolve_text(def), rc, def.font_index, color);
        }
    }
}

std::wstring window_text(const sphere::client::UiWindowDef& window) {
    const auto name = lowercase(window.name);
    if ((name == L"puppet" || name == L"statinfo") && !g_app->game_world.character_name.empty()) {
        return g_app->game_world.character_name;
    }
    if (window.text_key.empty()) {
        return {};
    }
    const auto& value = sphere::client::lookup_ui_string(g_app->runtime.strings, window.text_key);
    return value.empty() && lowercase(window.text_key).starts_with(L"uistr_") ? L"" : (value.empty() ? window.text_key : value);
}

RECT ui_window_rect(HWND hwnd, const sphere::client::UiWindowDef& window) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int client_w = client.right - client.left;
    const int client_h = client.bottom - client.top;

    int x = window.x;
    int y = window.y;
    if (window.align_right_x) {
        x = client_w - window.width + window.x;
    } else if (window.align_center_x) {
        x = (client_w - window.width) / 2 + window.x;
    }
    if (window.align_center_y) {
        y = (client_h - window.height) / 2 + window.y;
    } else if (window.align_right_y) {
        y = client_h - window.height + window.y;
    }
    return RECT{x, y, x + window.width, y + window.height};
}

RECT ui_window_title_rect(HWND hwnd, const sphere::client::UiWindowDef& window) {
    const RECT rect = ui_window_rect(hwnd, window);
    return RECT{
        rect.left + window.title_left,
        rect.top + window.title_top,
        rect.left + window.title_right,
        rect.top + window.title_bottom,
    };
}

bool ui_window_title_hit(HWND hwnd, const sphere::client::UiWindowDef& window, POINT pt) {
    if (window.title_right <= window.title_left || window.title_bottom <= window.title_top) {
        return false;
    }
    const RECT title = ui_window_title_rect(hwnd, window);
    return PtInRect(&title, pt) != FALSE;
}

void make_ui_window_position_absolute(HWND hwnd, sphere::client::UiWindowDef& window) {
    const RECT rect = ui_window_rect(hwnd, window);
    window.x = rect.left;
    window.y = rect.top;
    window.align_center_x = false;
    window.align_center_y = false;
    window.align_right_x = false;
    window.align_right_y = false;
}

void move_ui_window(HWND hwnd, sphere::client::UiWindowDef& window, int dx, int dy) {
    window.x += dx;
    window.y += dy;
    if (!window.can_not_cross) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    const int max_x = (std::max)(0, static_cast<int>(client.right - client.left) - window.width);
    const int max_y = (std::max)(0, static_cast<int>(client.bottom - client.top) - window.height);
    window.x = std::clamp(window.x, 0, max_x);
    window.y = std::clamp(window.y, 0, max_y);
}

void begin_ui_drag(HWND hwnd, sphere::client::UiWindowDef& window, UiDragKind kind, int game_window_index, POINT pt) {
    make_ui_window_position_absolute(hwnd, window);
    g_app->ui_drag_kind = kind;
    g_app->ui_drag_game_window_index = game_window_index;
    g_app->ui_drag_last_mouse = pt;
    SetCapture(hwnd);
}

void end_ui_drag() {
    g_app->ui_drag_kind = UiDragKind::None;
    g_app->ui_drag_game_window_index = -1;
}

std::wstring format_game_spawn_line() {
    const auto& world = g_app->game_world;
    std::wostringstream out;
    out << L"> ";
    if (!world.character_name.empty()) {
        out << world.character_name << L": ";
    }
    if (world.has_spawn) {
        out << L"spawn x=" << std::fixed << std::setprecision(1) << world.x
            << L" y=" << world.y
            << L" z=" << world.z
            << L" angle=" << std::setprecision(2) << world.angle;
    } else {
        out << L"spawn packet is not decoded";
    }
    return out.str();
}

std::wstring format_game_packet_line() {
    const auto& world = g_app->game_world;
    std::wostringstream out;
    out << L"> packets character=" << world.select_packet_count << L"/" << world.select_byte_count
        << L" world=" << world.world_packet_count << L"/" << world.world_byte_count;
    return out.str();
}

double game_progress_ratio(const sphere::client::UiControlDef& def) {
    const auto& slot = g_app->character_slots[static_cast<std::size_t>(std::clamp(g_app->game_world.selected_slot, 0, 2))];
    const auto sprite = lowercase(def.draw_sprite_name);
    if (sprite == L"blue" && slot.max_mp > 0) {
        return std::clamp(static_cast<double>(slot.current_mp) / static_cast<double>(slot.max_mp), 0.0, 1.0);
    }
    if (slot.max_hp > 0) {
        return std::clamp(static_cast<double>(slot.current_hp) / static_cast<double>(slot.max_hp), 0.0, 1.0);
    }
    return 1.0;
}

COLORREF game_progress_color(const sphere::client::UiControlDef& def) {
    const auto sprite = lowercase(def.draw_sprite_name);
    if (sprite == L"blue") {
        return RGB(40, 105, 210);
    }
    if (sprite == L"yellow" || sprite == L"yellow1") {
        return RGB(215, 190, 40);
    }
    return RGB(70, 180, 60);
}

void draw_game_progress(HDC dc, const sphere::client::UiControlDef& def, RECT rc) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }

    HBRUSH background = CreateSolidBrush(RGB(30, 28, 24));
    FillRect(dc, &rc, background);
    DeleteObject(background);

    RECT fill = rc;
    fill.right = fill.left + static_cast<int>((fill.right - fill.left) * game_progress_ratio(def));
    if (fill.right > fill.left) {
        HBRUSH brush = CreateSolidBrush(game_progress_color(def));
        FillRect(dc, &fill, brush);
        DeleteObject(brush);
    }
}

void draw_game_textlist(
    HDC dc,
    const sphere::client::UiWindowDef& window,
    const sphere::client::UiControlDef& def,
    RECT rc) {
    const auto name = lowercase(window.name);
    if (name == L"chat_sys") {
        const int line_height = max(14, def.height / 2);
        RECT first{rc.left, rc.top, rc.right, min(rc.bottom, rc.top + line_height)};
        RECT second{rc.left, first.bottom, rc.right, rc.bottom};
        draw_ui_text(dc, format_game_spawn_line(), first, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, sphere::client::UiColor{170, 170, 170, 255});
        draw_ui_text(dc, format_game_packet_line(), second, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, sphere::client::UiColor{135, 135, 135, 255});
        return;
    }
    if (name != L"chat" && name != L"chat_st2") {
        return;
    }

    const int line_height = 14;
    const int visible_lines = (std::max)(1, static_cast<int>(rc.bottom - rc.top) / line_height);
    const int first_line = (std::max)(0, static_cast<int>(g_app->game_chat_lines.size()) - visible_lines);
    int y = rc.top;
    for (int index = first_line; index < static_cast<int>(g_app->game_chat_lines.size()) && y < rc.bottom; ++index) {
        RECT line{rc.left, y, rc.right, (std::min)(rc.bottom, static_cast<LONG>(y + line_height))};
        draw_ui_text(dc, g_app->game_chat_lines[static_cast<std::size_t>(index)], line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.text_color);
        y += line_height;
    }
}

std::wstring plain_hypertext(const std::wstring& source) {
    std::wstring without_tags;
    without_tags.reserve(source.size());
    for (std::size_t index = 0; index < source.size();) {
        if (source[index] != L'<') {
            without_tags.push_back(source[index++]);
            continue;
        }
        const auto end = source.find(L'>', index + 1);
        if (end == std::wstring::npos) {
            break;
        }
        const auto tag = lowercase(source.substr(index + 1, end - index - 1));
        if (tag == L"br") {
            without_tags.push_back(L'\n');
        } else if (tag == L"tab") {
            without_tags.append(4, L' ');
        }
        index = end + 1;
    }

    std::wstring result;
    std::wistringstream lines(without_tags);
    std::wstring line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(L" \t\r");
        const auto last = line.find_last_not_of(L" \t\r");
        const auto trimmed = first == std::wstring::npos ? std::wstring{} : line.substr(first, last - first + 1);
        if (trimmed.empty() || trimmed == L"hypertext" || trimmed == L"{" || trimmed == L"}" ||
            trimmed.starts_with(L"//")) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(L'\n');
        }
        result += line;
    }
    return result;
}

std::wstring enabled_text(bool enabled) {
    return ui_string(enabled ? L"UISTR_WT_OPT16" : L"UISTR_WT_OPT17");
}

std::wstring quality_text(int value, bool has_off) {
    if (has_off && value <= 0) {
        return enabled_text(false);
    }
    const int quality = std::clamp(value - (has_off ? 1 : 0), 0, 3);
    const wchar_t* keys[] = {L"UISTR_WT_OPT18", L"UISTR_WT_OPT19", L"UISTR_WT_OPT20", L"UISTR_WT_OPT21"};
    return ui_string(keys[quality]);
}

std::wstring game_control_text(const sphere::client::UiWindowDef& window, const sphere::client::UiControlDef& def) {
    const auto name = lowercase(window.name);
    if (name == L"gfx_options") {
        switch (def.id) {
        case 7: {
            std::wostringstream text;
            text << g_app->settings.width << L"x" << g_app->settings.height << L" " << g_app->settings.depth;
            return text.str();
        }
        case 9:
            return quality_text(g_app->settings.shadow_quality, true);
        case 10: {
            const auto& labels = g_app->lua_boot.game_window.grass_mode_text;
            const auto mode = static_cast<std::size_t>(std::clamp(g_app->settings.grass_quality, 0, 2));
            return mode < labels.size() ? labels[mode] : L"";
        }
        case 22:
            return quality_text(g_app->settings.reflection_quality, false);
        case 31:
            return enabled_text(g_app->settings.effects_quality != 0);
        case 35:
            return ui_string(g_app->settings.windowed ? L"UISTR_WT_OPT61" : L"UISTR_WT_OPT62");
        case 37:
            return enabled_text(g_app->settings.auto_fog);
        case 41:
            return enabled_text(g_app->settings.lods);
        case 49:
            return enabled_text(g_app->settings.post_effects);
        default:
            break;
        }
    } else if (name == L"sound_options" && def.id == 9) {
        return ui_string(g_app->settings.hardware_mix ? L"UISTR_WT_OPT34" : L"UISTR_WT_OPT33");
    }
    if (def.text_key.empty()) {
        return {};
    }
    const auto& value = sphere::client::lookup_ui_string(g_app->runtime.strings, def.text_key);
    return value.empty() ? def.text_key : value;
}

RECT game_control_rect(RECT window_rect, const sphere::client::UiControlDef& def) {
    RECT rc = control_rect_at(window_rect, def);
    if (class_is(def, L"SpinButton") && (def.width <= 0 || def.height <= 0)) {
        rc.right = rc.left + 37;
        rc.bottom = rc.top + 26;
    }
    return rc;
}

std::uint64_t game_control_state_key(int window_index, int control_id) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(window_index)) << 32) |
        static_cast<std::uint32_t>(control_id);
}

bool game_control_is_checked(int window_index, int control_id) {
    const auto found = g_app->game_control_checked.find(game_control_state_key(window_index, control_id));
    return found != g_app->game_control_checked.end() && found->second;
}

RECT scroll_sub_button_rect(
    RECT control_rect,
    const sphere::client::UiSubButtonDef& button) {
    return RECT{
        control_rect.left + button.x,
        control_rect.top + button.y,
        control_rect.left + button.x + button.width,
        control_rect.top + button.y + button.height,
    };
}

double game_scroll_ratio(const sphere::client::UiWindowDef& window, int control_id) {
    const auto name = lowercase(window.name);
    if (name == L"sound_options") {
        if (control_id == 7) {
            return static_cast<double>(g_app->settings.music_volume) / 100.0;
        }
        if (control_id == 8) {
            return static_cast<double>(g_app->settings.sound_volume) / 100.0;
        }
    } else if (name == L"gfx_options") {
        if (control_id == 28) {
            return static_cast<double>(g_app->settings.fog_distance - 30) / 170.0;
        }
        if (control_id == 46) {
            return static_cast<double>(g_app->settings.lod_distance) / 200.0;
        }
    }
    return 0.0;
}

void draw_game_control(HDC dc, const sphere::client::UiWindowDef& window, int window_index, RECT window_rect, const sphere::client::UiControlDef& def) {
    if (def.hidden) {
        return;
    }

    RECT rc = game_control_rect(window_rect, def);
    if (class_is(def, L"Image")) {
        if (!def.image_name.empty()) {
            draw_sprite_from_window(dc, window, def.image_name, rc.left, rc.top);
        }
        return;
    }
    if (class_is(def, L"Slot")) {
        return;
    }
    if (class_is(def, L"CheckBox")) {
        const bool pressed = g_app->game_pressed_window_index == window_index && g_app->game_pressed_control_id == def.id;
        const bool hot = g_app->game_hot_window_index == window_index && g_app->game_hot_control_id == def.id;
        const bool checked = game_control_is_checked(window_index, def.id);
        const auto& sprite = checked || pressed
            ? def.checked_image
            : (hot && !def.focused_image.empty() ? def.focused_image : def.unchecked_image);
        if (!sprite.empty()) {
            draw_sprite_from_window(dc, window, sprite, rc.left, rc.top);
        }
        return;
    }
    if (class_is(def, L"Button")) {
        const bool pressed = g_app->game_pressed_window_index == window_index && g_app->game_pressed_control_id == def.id;
        const bool hot = g_app->game_hot_window_index == window_index && g_app->game_hot_control_id == def.id;
        std::wstring sprite;
        if (pressed && !def.checked_image.empty()) {
            sprite = def.checked_image;
        } else if (hot && !def.focused_image.empty()) {
            sprite = def.focused_image;
        } else {
            sprite = def.unchecked_image;
        }
        if (!sprite.empty()) {
            draw_sprite_from_window(dc, window, sprite, rc.left, rc.top);
        }
        const auto text = game_control_text(window, def);
        if (!text.empty()) {
            draw_text_centered(dc, text, rc, def.font_index, def.disabled ? def.disabled_color : def.text_color);
        }
        return;
    }
    if (class_is(def, L"SpinButton")) {
        const bool pressed = g_app->game_pressed_window_index == window_index && g_app->game_pressed_control_id == def.id;
        const bool hot = g_app->game_hot_window_index == window_index && g_app->game_hot_control_id == def.id;
        const int middle = rc.left + (rc.right - rc.left) / 2;
        const bool left_hot = hot && g_app->game_pressed_mouse.x < middle;
        const bool left_pressed = pressed && g_app->game_pressed_mouse.x < middle;
        const bool right_hot = hot && !left_hot;
        const bool right_pressed = pressed && !left_pressed;
        draw_sprite_from_window(dc, window, left_pressed ? L"sp_lpush" : (left_hot ? L"sp_lfocus" : L"sp_lnormal"), rc.left + 1, rc.top + 4);
        draw_sprite_from_window(dc, window, right_pressed ? L"sp_rpush" : (right_hot ? L"sp_rfocus" : L"sp_rnormal"), rc.left + 19, rc.top + 4);
        return;
    }
    if (class_is(def, L"Scroll_Bar") || class_is(def, L"SCROLL_BAR")) {
        if (def.scroll_sprite_name.empty() || def.scroll_sprite_width <= 0 || def.scroll_sprite_height <= 0) {
            return;
        }
        const double ratio = std::clamp(game_scroll_ratio(window, def.id), 0.0, 1.0);
        const int thumb_width = def.scroll_sprite_width;
        const int thumb_height = def.scroll_sprite_height;
        const int x = rc.left + static_cast<int>(std::round((rc.right - rc.left) * ratio)) - thumb_width / 2;
        const int y = rc.top + (rc.bottom - rc.top - thumb_height) / 2;
        draw_sprite_from_window(dc, window, def.scroll_sprite_name, x, y);

        const bool pressed = g_app->game_pressed_window_index == window_index && g_app->game_pressed_control_id == def.id;
        const bool hot = g_app->game_hot_window_index == window_index && g_app->game_hot_control_id == def.id;
        const RECT left = scroll_sub_button_rect(rc, def.left_button);
        const RECT right = scroll_sub_button_rect(rc, def.right_button);
        const bool left_hot = hot && PtInRect(&left, g_app->game_pressed_mouse);
        const bool right_hot = hot && PtInRect(&right, g_app->game_pressed_mouse);
        if (left_hot) {
            draw_sprite_from_window(
                dc,
                window,
                pressed ? def.left_button.checked_image : def.left_button.focused_image,
                left.left,
                left.top);
        }
        if (right_hot) {
            draw_sprite_from_window(
                dc,
                window,
                pressed ? def.right_button.checked_image : def.right_button.focused_image,
                right.left,
                right.top);
        }
        return;
    }
    if (class_is(def, L"Progress_Bar") || class_is(def, L"ProgressBar")) {
        draw_game_progress(dc, def, rc);
        return;
    }
    if (class_is(def, L"TextList") || class_is(def, L"HTCHATLISTCTRL")) {
        draw_game_textlist(dc, window, def, rc);
        return;
    }
    if (class_is(def, L"Edit") || class_is(def, L"HTEDIT")) {
        std::wstring text;
        if (g_app->game_active_edit_window_index == window_index && g_app->game_active_edit_control_id == def.id) {
            text = g_app->game_edit_text + L"_";
        }
        const UINT align = def.text_center ? DT_CENTER : DT_LEFT;
        draw_ui_text(dc, text, rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.disabled ? def.disabled_color : def.text_color);
        return;
    }
    if (class_is(def, L"HYPER_TEXT")) {
        if (lowercase(window.name) == L"help" && !g_app->game_help_text.empty()) {
            draw_ui_text(dc, g_app->game_help_text, rc, DT_LEFT | DT_TOP | DT_WORDBREAK, def.font_index, def.text_color);
        }
        return;
    }
    if (class_is(def, L"Text")) {
        const UINT align = def.text_center ? DT_CENTER : DT_LEFT;
        draw_ui_text(dc, game_control_text(window, def), rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.disabled ? def.disabled_color : def.text_color);
    }
}

void draw_game_window(HDC dc, HWND hwnd, const sphere::client::UiWindowDef& window, int window_index) {
    if (window_index >= 0 && window_index < static_cast<int>(g_app->game_window_visible.size()) &&
        !g_app->game_window_visible[static_cast<std::size_t>(window_index)]) {
        return;
    }
    const RECT rect = ui_window_rect(hwnd, window);
    if (!window.draw_none && !window.draw_sprite_name.empty()) {
        draw_sprite_from_window(dc, window, window.draw_sprite_name, rect.left, rect.top);
    }
    const auto title = window_text(window);
    if (!title.empty() && window.title_bottom > window.title_top) {
        RECT title_rc{
            rect.left + window.title_left,
            rect.top + window.title_top,
            rect.left + window.title_right,
            rect.top + window.title_bottom,
        };
        draw_text_centered(dc, title, title_rc, window.font_index, window.text_color);
    }
    for (const auto& control : window.controls) {
        draw_game_control(dc, window, window_index, rect, control);
    }
}

std::pair<int, int> game_hit_test(HWND hwnd, POINT pt) {
    for (auto order = g_app->game_window_z_order.rbegin(); order != g_app->game_window_z_order.rend(); ++order) {
        const int i = *order;
        if (i < static_cast<int>(g_app->game_window_visible.size()) &&
            !g_app->game_window_visible[static_cast<std::size_t>(i)]) {
            continue;
        }
        const auto& window = g_app->game_windows[static_cast<std::size_t>(i)];
        const RECT window_rect = ui_window_rect(hwnd, window);
        if (!PtInRect(&window_rect, pt)) {
            continue;
        }
        for (const auto& control : window.controls) {
            if (control.hidden || control.disabled ||
                (!class_is(control, L"Button") && !class_is(control, L"SpinButton") &&
                 !class_is(control, L"Scroll_Bar") && !class_is(control, L"SCROLL_BAR") &&
                 !class_is(control, L"CheckBox") && !class_is(control, L"Edit") &&
                 !class_is(control, L"HTEDIT"))) {
                continue;
            }
            const RECT rc = game_control_rect(window_rect, control);
            bool hit = PtInRect(&rc, pt) != FALSE;
            if (!hit && (class_is(control, L"Scroll_Bar") || class_is(control, L"SCROLL_BAR"))) {
                const RECT left = scroll_sub_button_rect(rc, control.left_button);
                const RECT right = scroll_sub_button_rect(rc, control.right_button);
                hit = PtInRect(&left, pt) != FALSE || PtInRect(&right, pt) != FALSE;
            }
            if (hit) {
                return {i, control.id};
            }
        }
    }
    return {-1, 0};
}

int game_window_index_by_name(const std::wstring& name) {
    const auto expected = lowercase(name);
    for (std::size_t index = 0; index < g_app->game_windows.size(); ++index) {
        if (lowercase(g_app->game_windows[index].name) == expected) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void bring_game_window_to_front(int index) {
    if (index < 0 || index >= static_cast<int>(g_app->game_windows.size()) ||
        !g_app->game_windows[static_cast<std::size_t>(index)].can_go_top) {
        return;
    }
    auto found = std::find(g_app->game_window_z_order.begin(), g_app->game_window_z_order.end(), index);
    if (found == g_app->game_window_z_order.end() || std::next(found) == g_app->game_window_z_order.end()) {
        return;
    }
    g_app->game_window_z_order.erase(found);
    g_app->game_window_z_order.push_back(index);
}

void set_game_window_visible(const std::wstring& name, bool visible) {
    const int index = game_window_index_by_name(name);
    if (index < 0 || index >= static_cast<int>(g_app->game_window_visible.size())) {
        return;
    }
    g_app->game_window_visible[static_cast<std::size_t>(index)] = visible;
    if (visible) {
        bring_game_window_to_front(index);
    } else if (g_app->game_active_edit_window_index == index) {
        g_app->game_active_edit_window_index = -1;
        g_app->game_active_edit_control_id = 0;
        g_app->game_edit_text.clear();
    }
}

void toggle_game_window(const std::wstring& name) {
    const int index = game_window_index_by_name(name);
    if (index < 0 || index >= static_cast<int>(g_app->game_window_visible.size())) {
        return;
    }
    set_game_window_visible(name, !g_app->game_window_visible[static_cast<std::size_t>(index)]);
}

void toggle_game_chat() {
    const int compact = game_window_index_by_name(L"chat");
    const int expanded = game_window_index_by_name(L"chat_st2");
    if (compact < 0 || expanded < 0) {
        return;
    }
    const bool expanded_visible = g_app->game_window_visible[static_cast<std::size_t>(expanded)];
    set_game_window_visible(L"chat_st2", !expanded_visible);
    set_game_window_visible(L"chat", expanded_visible);
}

bool execute_lua_ui_action(int window_index, int control_id) {
    const auto window_name = lowercase(g_app->game_windows[static_cast<std::size_t>(window_index)].name);
    const auto found = std::find_if(
        g_app->lua_boot.game_window.ui_actions.begin(),
        g_app->lua_boot.game_window.ui_actions.end(),
        [&](const sphere::client::LuaUiAction& action) {
            return lowercase(action.window) == window_name && action.control == control_id;
        });
    if (found == g_app->lua_boot.game_window.ui_actions.end()) {
        return false;
    }

    const auto action = lowercase(found->action);
    if (action == L"toggle_window") {
        toggle_game_window(found->target);
    } else if (action == L"show_window") {
        set_game_window_visible(found->target, true);
    } else if (action == L"hide_window") {
        set_game_window_visible(found->target, false);
    } else if (action == L"swap_window") {
        set_game_window_visible(g_app->game_windows[static_cast<std::size_t>(window_index)].name, false);
        set_game_window_visible(found->target, true);
    } else if (action == L"toggle_chat") {
        toggle_game_chat();
    } else if (action == L"cycle_pair") {
        const int first = game_window_index_by_name(found->target);
        const int second = game_window_index_by_name(found->alternate);
        if (first >= 0 && second >= 0) {
            const bool first_visible = g_app->game_window_visible[static_cast<std::size_t>(first)];
            const bool second_visible = g_app->game_window_visible[static_cast<std::size_t>(second)];
            set_game_window_visible(found->target, !first_visible && !second_visible);
            set_game_window_visible(found->alternate, first_visible);
        }
    } else {
        return false;
    }
    return true;
}

bool close_topmost_game_window() {
    for (auto order = g_app->game_window_z_order.rbegin(); order != g_app->game_window_z_order.rend(); ++order) {
        const int index = *order;
        if (index < 0 || index >= static_cast<int>(g_app->game_window_visible.size()) ||
            !g_app->game_window_visible[static_cast<std::size_t>(index)]) {
            continue;
        }
        const auto& window = g_app->game_windows[static_cast<std::size_t>(index)];
        if (!window.escape_handle || index >= g_app->settings_window_start_index) {
            continue;
        }
        set_game_window_visible(window.name, false);
        return true;
    }
    return false;
}

void hide_settings_windows() {
    if (g_app->settings_window_start_index < 0) {
        return;
    }
    for (int index = g_app->settings_window_start_index; index < static_cast<int>(g_app->game_window_visible.size()); ++index) {
        g_app->game_window_visible[static_cast<std::size_t>(index)] = false;
    }
}

void show_settings_window(const std::wstring& name) {
    hide_settings_windows();
    const int index = game_window_index_by_name(name);
    if (index >= g_app->settings_window_start_index && index < static_cast<int>(g_app->game_window_visible.size())) {
        g_app->game_window_visible[static_cast<std::size_t>(index)] = true;
        bring_game_window_to_front(index);
    }
}

std::vector<std::array<int, 3>> available_display_modes() {
    std::vector<std::array<int, 3>> modes;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode); ++index) {
        const std::array<int, 3> value{
            static_cast<int>(mode.dmPelsWidth),
            static_cast<int>(mode.dmPelsHeight),
            static_cast<int>(mode.dmBitsPerPel),
        };
        if (std::find(modes.begin(), modes.end(), value) == modes.end()) {
            modes.push_back(value);
        }
    }
    std::sort(modes.begin(), modes.end());
    return modes;
}

void cycle_display_mode(int delta) {
    const auto modes = available_display_modes();
    if (modes.empty()) {
        return;
    }
    const std::array<int, 3> current{g_app->settings.width, g_app->settings.height, g_app->settings.depth};
    auto it = std::find(modes.begin(), modes.end(), current);
    int index = it == modes.end() ? 0 : static_cast<int>(std::distance(modes.begin(), it));
    index = std::clamp(index + delta, 0, static_cast<int>(modes.size()) - 1);
    g_app->settings.width = modes[static_cast<std::size_t>(index)][0];
    g_app->settings.height = modes[static_cast<std::size_t>(index)][1];
    g_app->settings.depth = modes[static_cast<std::size_t>(index)][2];
}

void restore_settings_dialog_backup() {
    if (!g_app->settings_dialog_backup) {
        return;
    }
    const auto root = g_app->settings.root;
    const bool debug_auto_enter = g_app->settings.debug_auto_enter;
    const bool debug_character = g_app->settings.debug_start_character_scene;
    const bool debug_world = g_app->settings.debug_start_game_world;
    g_app->settings = *g_app->settings_dialog_backup;
    g_app->settings.root = root;
    g_app->settings.debug_auto_enter = debug_auto_enter;
    g_app->settings.debug_start_character_scene = debug_character;
    g_app->settings.debug_start_game_world = debug_world;
    apply_music_volume();
    apply_grass_quality();
}

void toggle_game_options(HWND hwnd) {
    const int index = game_window_index_by_name(L"options");
    if (index < 0 || index >= static_cast<int>(g_app->game_window_visible.size())) {
        return;
    }
    if (g_app->game_window_visible[static_cast<std::size_t>(index)]) {
        hide_settings_windows();
    } else {
        show_settings_window(L"options");
    }
    update_game_overlay(hwnd);
}

void activate_game_control(HWND hwnd, int window_index, int control_id, POINT pt) {
    if (window_index < 0 || window_index >= static_cast<int>(g_app->game_windows.size())) {
        return;
    }
    const auto& window = g_app->game_windows[static_cast<std::size_t>(window_index)];
    const auto window_name = lowercase(window.name);
    const auto control = std::find_if(window.controls.begin(), window.controls.end(), [control_id](const auto& value) {
        return value.id == control_id;
    });
    if (control == window.controls.end()) {
        return;
    }

    if (execute_lua_ui_action(window_index, control_id)) {
        update_game_overlay(hwnd);
        return;
    }

    if (window_name == L"options") {
        switch (control_id) {
        case 1:
        case 9:
            hide_settings_windows();
            break;
        case 3:
            g_app->settings_dialog_backup = g_app->settings;
            show_settings_window(L"gfx_options");
            break;
        case 4:
            g_app->settings_dialog_backup = g_app->settings;
            show_settings_window(L"sound_options");
            break;
        case 5:
            g_app->settings_dialog_backup = g_app->settings;
            show_settings_window(L"control_options");
            break;
        case 6:
            g_app->settings_dialog_backup = g_app->settings;
            show_settings_window(L"interface_options");
            break;
        case 7:
            show_settings_window(L"authors");
            break;
        case 8:
            DestroyWindow(hwnd);
            return;
        default:
            break;
        }
    } else if (window_index >= g_app->settings_window_start_index) {
        if (control_id == 1) {
            if (window_name == L"gfx_options") {
                save_graphics_settings();
            } else if (window_name == L"sound_options") {
                save_sound_settings();
            }
            g_app->settings_dialog_backup.reset();
            show_settings_window(L"options");
        } else if (control->send_quit || control_id == 2) {
            restore_settings_dialog_backup();
            g_app->settings_dialog_backup.reset();
            show_settings_window(L"options");
        } else if (window_name == L"gfx_options" && class_is(*control, L"SpinButton")) {
            const RECT window_rect = ui_window_rect(hwnd, window);
            const RECT rc = game_control_rect(window_rect, *control);
            const int delta = pt.x < rc.left + (rc.right - rc.left) / 2 ? -1 : 1;
            switch (control_id) {
            case 15:
                cycle_display_mode(delta);
                break;
            case 17:
                g_app->settings.shadow_quality = std::clamp(g_app->settings.shadow_quality + delta, 0, 4);
                break;
            case 18:
                g_app->settings.grass_quality = std::clamp(g_app->settings.grass_quality + delta, 0, 2);
                apply_grass_quality();
                break;
            case 24:
                g_app->settings.reflection_quality = std::clamp(g_app->settings.reflection_quality + delta, 0, 3);
                break;
            case 26:
                g_app->settings.effects_quality = std::clamp(g_app->settings.effects_quality + delta, 0, 1);
                break;
            case 34:
                g_app->settings.windowed = !g_app->settings.windowed;
                break;
            case 39:
                g_app->settings.auto_fog = !g_app->settings.auto_fog;
                break;
            case 43:
                g_app->settings.lods = !g_app->settings.lods;
                break;
            case 51:
                g_app->settings.post_effects = !g_app->settings.post_effects;
                break;
            default:
                break;
            }
        } else if (window_name == L"gfx_options" &&
                   (class_is(*control, L"Scroll_Bar") || class_is(*control, L"SCROLL_BAR"))) {
            const RECT rc = game_control_rect(ui_window_rect(hwnd, window), *control);
            const RECT left = scroll_sub_button_rect(rc, control->left_button);
            const RECT right = scroll_sub_button_rect(rc, control->right_button);
            const int direction = PtInRect(&left, pt) ? -1 : (PtInRect(&right, pt) ? 1 : 0);
            const double ratio = std::clamp(static_cast<double>(pt.x - rc.left) / static_cast<double>((std::max)(1L, rc.right - rc.left)), 0.0, 1.0);
            if (control_id == 28) {
                g_app->settings.fog_distance = direction == 0
                    ? 30 + static_cast<int>(std::round(ratio * 170.0))
                    : std::clamp(g_app->settings.fog_distance + direction * control->delta_step, 30, 200);
            } else if (control_id == 46) {
                g_app->settings.lod_distance = direction == 0
                    ? static_cast<int>(std::round(ratio * 200.0))
                    : std::clamp(g_app->settings.lod_distance + direction * control->delta_step, 0, 200);
            }
        } else if (window_name == L"sound_options" && control_id == 9) {
            g_app->settings.hardware_mix = !g_app->settings.hardware_mix;
        } else if (window_name == L"sound_options" &&
                   (class_is(*control, L"Scroll_Bar") || class_is(*control, L"SCROLL_BAR"))) {
            const RECT rc = game_control_rect(ui_window_rect(hwnd, window), *control);
            const RECT left = scroll_sub_button_rect(rc, control->left_button);
            const RECT right = scroll_sub_button_rect(rc, control->right_button);
            const int direction = PtInRect(&left, pt) ? -1 : (PtInRect(&right, pt) ? 1 : 0);
            const int value = std::clamp(static_cast<int>(std::round(
                static_cast<double>(pt.x - rc.left) / static_cast<double>((std::max)(1L, rc.right - rc.left)) * 100.0)), 0, 100);
            if (control_id == 7) {
                g_app->settings.music_volume = direction == 0
                    ? value
                    : std::clamp(g_app->settings.music_volume + direction * control->delta_step, 0, 100);
                apply_music_volume();
            } else if (control_id == 8) {
                g_app->settings.sound_volume = direction == 0
                    ? value
                    : std::clamp(g_app->settings.sound_volume + direction * control->delta_step, 0, 100);
            }
        }
    } else if (class_is(*control, L"CheckBox")) {
        const auto key = game_control_state_key(window_index, control_id);
        g_app->game_control_checked[key] = !game_control_is_checked(window_index, control_id);
    } else if (class_is(*control, L"Edit") || class_is(*control, L"HTEDIT")) {
        g_app->game_active_edit_window_index = window_index;
        g_app->game_active_edit_control_id = control_id;
        g_app->game_edit_text.clear();
    } else if (control->send_help) {
        if (!control->window_help.empty()) {
            const auto path = g_app->settings.root / control->window_help;
            if (!std::filesystem::exists(path)) {
                g_app->status = L"Help file is missing: " + path.wstring();
            } else {
                g_app->game_help_text = plain_hypertext(sphere::client::decode_cp1251_file(path));
                set_game_window_visible(L"help", true);
            }
        }
    } else if (control->send_quit) {
        set_game_window_visible(window.name, false);
    }
    update_game_overlay(hwnd);
}

RECT character_control_rect(RECT window_rect, const sphere::client::UiControlDef& def) {
    RECT rc = control_rect_at(window_rect, def);
    if (class_is(def, L"SpinButton") && (def.width <= 0 || def.height <= 0)) {
        rc.right = rc.left + 37;
        rc.bottom = rc.top + 26;
    }
    return rc;
}

const sphere::client::UiControlDef* character_control_by_id(int id) {
    for (const auto& control : g_app->pick_person_window.controls) {
        if (!control.hidden && !control.disabled && control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

sphere::client::UiControlDef* mutable_character_control_by_id(int id) {
    for (auto& control : g_app->pick_person_window.controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

int selected_slot_index() {
    return std::clamp(g_app->selected_character_slot, 0, 2);
}

const sphere::client::CharacterSlot& selected_character_slot() {
    return g_app->character_slots[static_cast<std::size_t>(selected_slot_index())];
}

std::wstring default_character_name_for_slot(int slot) {
    std::wstring name;
    for (wchar_t ch : g_app->login) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            name.push_back(ch);
        }
        if (name.size() >= 12) {
            break;
        }
    }
    if (name.empty()) {
        name = L"Hero";
    }
    if (name.front() >= L'0' && name.front() <= L'9') {
        name.insert(name.begin(), L'C');
    }
    if (slot > 0 && name.size() < 12) {
        name += static_cast<wchar_t>(L'1' + slot);
    }
    return name.substr(0, 12);
}

void sync_character_select_controls() {
    if (!g_app) {
        return;
    }

    const bool locked = g_app->character_action_in_progress || g_app->character_entered_game;
    const int selected = selected_slot_index();
    for (int i = 0; i < 3; ++i) {
        const auto& slot = g_app->character_slots[static_cast<std::size_t>(i)];
        if (auto* edit = mutable_character_control_by_id(60 + i)) {
            edit->hidden = i != selected || slot.present || !slot.can_create;
            edit->disabled = edit->hidden || locked;
        }
        if (auto* radio = mutable_character_control_by_id(63 + i)) {
            radio->hidden = false;
            radio->disabled = locked || (!slot.present && !slot.can_create);
        }
    }

    const auto& selected_slot = selected_character_slot();
    const bool can_edit_appearance = !selected_slot.present && selected_slot.can_create && !locked;
    for (int id = 12; id <= 16; ++id) {
        if (auto* control = mutable_character_control_by_id(id)) {
            control->hidden = false;
            control->disabled = !can_edit_appearance;
        }
    }

    if (auto* del = mutable_character_control_by_id(57)) {
        del->disabled = locked || !selected_slot.present;
    }
    if (auto* exit = mutable_character_control_by_id(58)) {
        exit->disabled = g_app->character_action_in_progress;
    }
    if (auto* cont = mutable_character_control_by_id(59)) {
        cont->disabled = locked || (!selected_slot.present && !selected_slot.can_create);
    }

    if (!selected_slot.present && selected_slot.can_create && !locked) {
        g_app->active_character_edit_id = 60 + selected;
    }
}

bool is_character_clickable_control(const sphere::client::UiControlDef& control) {
    return class_is(control, L"Button") || class_is(control, L"SpinButton") || class_is(control, L"RadioButton") || class_is(control, L"Edit");
}

int character_spin_delta_for_point(HWND hwnd, int control_id, POINT pt) {
    if (control_id < 12 || control_id > 16) {
        return 1;
    }
    const auto* control = character_control_by_id(control_id);
    if (!control) {
        return 1;
    }
    const RECT window_rect = ui_window_rect(hwnd, g_app->pick_person_window);
    const RECT rc = character_control_rect(window_rect, *control);
    return pt.x < rc.left + ((rc.right - rc.left) / 2) ? -1 : 1;
}

std::wstring empty_character_slot_text() {
    switch (g_app->settings.lang) {
    case 1:
        return L"Empty";
    case 3:
        return L"vazio";
    default:
        return L"\x041F\x0443\x0441\x0442\x043E";
    }
}

std::wstring character_gender_text(bool female) {
    if (female) {
        return g_app->settings.lang == 1 ? L"Female" : L"\x0416\x0435\x043D.";
    }
    return ui_string(L"UISTR_WT_CPS06");
}

int current_character_text_value(int id) {
    const auto& slot = selected_character_slot();
    const bool occupied = slot.present;
    switch (id) {
    case 17:
        return occupied ? slot.strength : g_app->appearance.strength;
    case 18:
        return occupied ? slot.dexterity : g_app->appearance.dexterity;
    case 19:
        return occupied ? slot.accuracy : g_app->appearance.accuracy;
    case 20:
        return occupied ? slot.endurance : g_app->appearance.endurance;
    case 29:
        return occupied ? slot.fire : g_app->appearance.fire;
    case 30:
        return occupied ? slot.water : g_app->appearance.water;
    case 31:
        return occupied ? slot.earth : g_app->appearance.earth;
    case 32:
        return occupied ? slot.air : g_app->appearance.air;
    case 43:
        return occupied ? slot.max_hp : 0;
    case 44:
        return occupied ? slot.max_mp : 0;
    case 48:
        return occupied ? slot.physical_attack : 0;
    case 49:
        return occupied ? slot.physical_defense : 0;
    case 50:
        return occupied ? slot.current_hp : 0;
    case 51:
        return occupied ? slot.magical_attack : 0;
    case 52:
        return occupied ? slot.magical_defense : 0;
    case 53:
        return occupied ? slot.current_mp : 0;
    default:
        return 0;
    }
}

std::wstring character_overlay_text(const sphere::client::UiControlDef& def) {
    if (g_app->mode == AppMode::CharacterSelect3D) {
        if (def.id >= 63 && def.id <= 65) {
            const int slot_index = def.id - 63;
            const auto& slot = g_app->character_slots[static_cast<std::size_t>(slot_index)];
            if (slot_index == selected_slot_index() && !slot.present && slot.can_create) {
                return {};
            }
            return slot.present ? slot.name : empty_character_slot_text();
        }
        if (def.id >= 60 && def.id <= 62) {
            const int slot_index = def.id - 60;
            auto text = g_app->character_name_edits[static_cast<std::size_t>(slot_index)];
            if (g_app->active_character_edit_id == def.id && !g_app->character_action_in_progress) {
                text += L"_";
            }
            return text;
        }
        if (def.id == 2) {
            const auto& slot = selected_character_slot();
            return character_gender_text(slot.present ? slot.female : g_app->appearance.gender != 0);
        }
        if ((def.id >= 17 && def.id <= 20) || (def.id >= 29 && def.id <= 32) || (def.id >= 43 && def.id <= 53)) {
            return std::to_wstring(current_character_text_value(def.id));
        }
        if (def.id == 55) {
            const auto& slot = selected_character_slot();
            return slot.present ? slot.name : std::wstring{};
        }
    }

    auto text = resolve_text(def);
    if (!text.empty() || g_app->mode != AppMode::CharacterSelect3D) {
        return text;
    }

    const auto& appearance = g_app->appearance;
    switch (def.id) {
    case 17:
        return std::to_wstring(appearance.strength);
    case 18:
        return std::to_wstring(appearance.dexterity);
    case 19:
        return std::to_wstring(appearance.accuracy);
    case 20:
        return std::to_wstring(appearance.endurance);
    case 29:
        return std::to_wstring(appearance.fire);
    case 30:
        return std::to_wstring(appearance.water);
    case 31:
        return std::to_wstring(appearance.earth);
    case 32:
        return std::to_wstring(appearance.air);
    default:
        return {};
    }
}

int character_hit_test(HWND hwnd, POINT pt, bool hot_only) {
    sync_character_select_controls();
    const RECT window_rect = ui_window_rect(hwnd, g_app->pick_person_window);
    if (!PtInRect(&window_rect, pt)) {
        return 0;
    }

    for (const auto& control : g_app->pick_person_window.controls) {
        if (control.hidden || control.disabled || !is_character_clickable_control(control)) {
            continue;
        }
        RECT rc = character_control_rect(window_rect, control);
        if (PtInRect(&rc, pt)) {
            return control.id;
        }
    }
    return hot_only ? -1 : 0;
}

int character_focus_for_control(int control_id) {
    if (control_id >= 12 && control_id <= 16) {
        return control_id;
    }
    if (control_id >= 7 && control_id <= 11) {
        return control_id + 5;
    }
    return 0;
}

void set_character_camera_focus(int focus_id) {
    if (focus_id == 0 || !g_app->character_scene) {
        return;
    }
    g_app->character_camera_focus_id = focus_id;
    g_app->character_scene->set_camera_focus(focus_id);
}

void draw_overlay_control(HDC dc, const sphere::client::UiWindowDef& window, RECT window_rect, const sphere::client::UiControlDef& def) {
    if (def.hidden) {
        return;
    }

    RECT rc = control_rect_at(window_rect, def);
    if (class_is(def, L"Text")) {
        const UINT align = def.text_center ? DT_CENTER : DT_LEFT;
        draw_ui_text(dc, character_overlay_text(def), rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.disabled ? def.disabled_color : def.text_color);
        return;
    }
    if (class_is(def, L"Edit")) {
        const UINT align = def.text_center ? DT_CENTER : DT_LEFT;
        draw_ui_text(dc, character_overlay_text(def), rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, def.disabled ? def.disabled_color : def.text_color);
        return;
    }
    if (class_is(def, L"Image")) {
        if (!def.image_name.empty()) {
            draw_sprite_from_window(dc, window, def.image_name, rc.left, rc.top);
        }
        return;
    }
    if (class_is(def, L"Progress_Bar") || class_is(def, L"ProgressBar")) {
        const bool blue = def.id == 42 || def.id == 46;
        const bool yellow = def.id == 47;
        const COLORREF color = yellow ? RGB(210, 190, 45) : (blue ? RGB(48, 109, 210) : RGB(70, 170, 60));
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        return;
    }
    if (class_is(def, L"RadioButton")) {
        const int slot_index = def.id - 63;
        if (slot_index == selected_slot_index() && !def.checked_image.empty()) {
            draw_sprite_from_window(dc, window, def.checked_image, rc.left - 10, rc.top + 4);
        }
        const int hot_id = g_app->character_hot_control_id;
        const auto& color = def.disabled ? def.disabled_color : (hot_id == def.id ? def.focus_color : def.text_color);
        draw_ui_text(dc, character_overlay_text(def), rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, def.font_index, color);
        return;
    }
    if (class_is(def, L"SpinButton")) {
        const bool pressed = g_app->character_pressed_control_id == def.id;
        const bool hot = g_app->character_hot_control_id == def.id;
        const bool left_side = g_app->character_spin_delta < 0;
        const std::wstring left_sprite = def.disabled ? L"sl_disabled" : (pressed && left_side ? L"sl_push" : (hot ? L"sl_focus" : L"sl_normal"));
        const std::wstring right_sprite = def.disabled ? L"sr_disabled" : (pressed && !left_side ? L"sr_push" : (hot ? L"sr_focus" : L"sr_normal"));
        draw_sprite_from_window(dc, window, left_sprite, rc.left + 1, rc.top + 4);
        draw_sprite_from_window(dc, window, right_sprite, rc.left + 19, rc.top + 4);
        return;
    }
    if (class_is(def, L"Button")) {
        const auto sprite = button_sprite_for(def);
        if (!sprite.empty()) {
            draw_sprite_from_window(dc, window, sprite, rc.left, rc.top);
        }
        const auto text = character_overlay_text(def);
        if (!text.empty()) {
            const int hot_id = g_app->mode == AppMode::CharacterSelect3D ? g_app->character_hot_control_id : g_app->hot_control_id;
            const auto& color = def.disabled ? def.disabled_color : (hot_id == def.id ? def.focus_color : def.text_color);
            draw_text_centered(dc, text, rc, def.font_index, color);
        }
    }
}

void draw_character_select_overlay(HDC dc, const sphere::client::UiWindowDef& window, RECT rect) {
    draw_sprite_from_window(dc, window, L"window", rect.left, rect.top);

    const int title_right = window.title_right > window.title_left
        ? min(window.title_right, window.width - window.title_left)
        : window.width - 10;
    RECT title_rc{
        rect.left + window.title_left,
        rect.top + window.title_top,
        rect.left + title_right,
        rect.top + (window.title_bottom > window.title_top ? window.title_bottom : 29),
    };
    draw_text_centered(dc, window_text(window), title_rc, window.font_index, window.text_color);

    for (const auto& control : window.controls) {
        draw_overlay_control(dc, window, rect, control);
    }
}

sphere::client::BitmapImage create_transparent_bitmap(int width, int height) {
    sphere::client::BitmapImage image;
    if (width <= 0 || height <= 0) {
        return image;
    }

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
        return image;
    }

    std::memset(pixels, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    image.handle = bitmap;
    image.pixels = static_cast<std::uint8_t*>(pixels);
    image.width = width;
    image.height = height;
    image.stride = width * 4;
    image.has_alpha = true;
    return image;
}

bool update_character_select_overlay(HWND hwnd) {
    g_app->character_overlay_dirty = false;
    sync_character_select_controls();
    const auto& window = g_app->pick_person_window;
    if (window.name.empty() || !g_app->character_scene) {
        return false;
    }

    auto bitmap = create_transparent_bitmap(window.width, window.height);
    if (!bitmap) {
        return false;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return false;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap.handle);
    HGDIOBJ old_font = nullptr;
    if (g_app->ui_font) {
        old_font = SelectObject(dc, g_app->ui_font);
    }

    RECT rect{0, 0, window.width, window.height};
    draw_character_select_overlay(dc, window, rect);

    if (old_font) {
        SelectObject(dc, old_font);
    }
    SelectObject(dc, old_bitmap);
    DeleteDC(dc);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height) * 4);
    for (int row = 0; row < bitmap.height; ++row) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.width) * 4,
            bitmap.pixels + static_cast<std::size_t>(row) * bitmap.stride,
            static_cast<std::size_t>(bitmap.width) * 4);
    }

    std::wstring error;
    return g_app->character_scene->set_overlay_bitmap(window.width, window.height, window.x, window.y, window.align_right_x, std::move(pixels), error);
}

bool update_game_overlay(HWND hwnd) {
    g_app->game_overlay_dirty = false;
    if (!g_app->game_scene) {
        return false;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    auto bitmap = create_transparent_bitmap(width, height);
    if (!bitmap) {
        return false;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return false;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap.handle);
    HGDIOBJ old_font = nullptr;
    if (g_app->ui_font) {
        old_font = SelectObject(dc, g_app->ui_font);
    }
    for (const int index : g_app->game_window_z_order) {
        draw_game_window(dc, hwnd, g_app->game_windows[static_cast<std::size_t>(index)], index);
    }
    if (old_font) {
        SelectObject(dc, old_font);
    }
    SelectObject(dc, old_bitmap);
    DeleteDC(dc);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4,
            bitmap.pixels + static_cast<std::size_t>(row) * bitmap.stride,
            static_cast<std::size_t>(width) * 4);
    }
    std::wstring error;
    if (!g_app->game_scene->set_overlay_bitmap(width, height, std::move(pixels), error)) {
        g_app->status = L"Game overlay update failed: " + error;
        return false;
    }
    return true;
}

void paint_character_select_overlay(HWND hwnd) {
    const auto& window = g_app->pick_person_window;
    if (window.name.empty()) {
        return;
    }

    HDC dc = GetDC(hwnd);
    if (!dc) {
        return;
    }

    const RECT rect = ui_window_rect(hwnd, window);
    draw_character_select_overlay(dc, window, rect);
    ReleaseDC(hwnd, dc);
}

void render_character_select_frame(HWND hwnd) {
    if (g_app->character_scene) {
        g_app->character_scene->render();
    }
}

void render_game_frame(HWND hwnd) {
    if (g_app->game_scene) {
        g_app->game_scene->render();
    }
}

void set_game_look_mode(HWND hwnd, bool enabled) {
    if (g_app->game_look_mode == enabled) {
        return;
    }
    g_app->game_look_mode = enabled;
    if (enabled) {
        RECT client{};
        GetClientRect(hwnd, &client);
        g_app->game_mouse_center = POINT{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        POINT screen = g_app->game_mouse_center;
        ClientToScreen(hwnd, &screen);
        SetCursorPos(screen.x, screen.y);
        SetCapture(hwnd);
        ShowCursor(FALSE);
    } else {
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        ShowCursor(TRUE);
    }
}

bool set_game_movement_key(WPARAM wparam, LPARAM lparam, bool pressed) {
    const int scan_code = static_cast<int>((static_cast<unsigned long long>(lparam) >> 16) & 0xffU);
    if (scan_code == g_app->settings.scan_forward) {
        g_app->game_movement.forward = pressed;
    } else if (scan_code == g_app->settings.scan_backward) {
        g_app->game_movement.backward = pressed;
    } else if (scan_code == g_app->settings.scan_strafe_left) {
        g_app->game_movement.strafe_left = pressed;
    } else if (scan_code == g_app->settings.scan_strafe_right) {
        g_app->game_movement.strafe_right = pressed;
    } else {
        return false;
    }
    return true;
}

void update_game_frame(HWND hwnd) {
    if (!g_app->game_scene) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_app->game_last_tick == 0) {
        g_app->game_last_tick = now;
    }
    const float delta_seconds = std::clamp(
        static_cast<float>(now - g_app->game_last_tick) / 1000.0f,
        0.0f,
        0.1f);
    g_app->game_last_tick = now;

    std::wstring update_error;
    if (!g_app->game_scene->update(delta_seconds, g_app->game_movement, update_error)) {
        g_app->status = update_error;
        append_client_log(L"game update failed: " + update_error);
    }

    const auto render_position = g_app->game_scene->position();
    const double old_x = g_app->game_world.x;
    const double old_y = g_app->game_world.y;
    const double old_z = g_app->game_world.z;
    const double old_angle = g_app->game_world.angle;
    g_app->game_world.x = render_position.x;
    g_app->game_world.y = -render_position.y;
    g_app->game_world.z = -render_position.z;
    g_app->game_world.angle = render_position.angle;
    const bool changed =
        std::abs(old_x - g_app->game_world.x) > 0.0001 ||
        std::abs(old_y - g_app->game_world.y) > 0.0001 ||
        std::abs(old_z - g_app->game_world.z) > 0.0001 ||
        std::abs(old_angle - g_app->game_world.angle) > 0.0001;

    if (changed && g_app->server_session &&
        now - g_app->game_last_position_send_tick >= static_cast<ULONGLONG>(g_app->lua_boot.game_window.position_send_interval_ms)) {
        std::string send_error;
        if (!g_app->server_session->send_position(
                render_position.x,
                render_position.y,
                render_position.z,
                render_position.angle,
                send_error)) {
            append_client_log(L"position send failed: " + widen_ascii(send_error));
        }
        g_app->game_last_position_send_tick = now;
    }
    if (g_app->server_session) {
        auto incoming = g_app->server_session->poll_frames();
        g_app->game_world.world_packet_count += incoming.packet_count;
        g_app->game_world.world_byte_count += incoming.byte_count;
    }
    if (changed && now - g_app->game_last_overlay_tick >= 200) {
        update_game_overlay(hwnd);
        g_app->game_last_overlay_tick = now;
    }
    render_game_frame(hwnd);
}

void draw_tips(HDC dc) {
    if (!g_app->tip.loaded) {
        return;
    }

    RECT title_rc{
        g_app->background_rect.left + 355,
        g_app->background_rect.top + 536,
        g_app->background_rect.left + 850,
        g_app->background_rect.top + 554,
    };
    RECT body_rc{
        g_app->background_rect.left + 355,
        g_app->background_rect.top + 554,
        g_app->background_rect.left + 850,
        g_app->background_rect.top + 620,
    };
    draw_ui_text(dc, g_app->tip.title, title_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, 5, sphere::client::UiColor{229, 185, 36, 255});
    draw_ui_text(dc, g_app->tip.body, body_rc, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS, 5, sphere::client::UiColor{83, 67, 34, 255});
}

void paint_scene(HWND hwnd, HDC dc) {
    update_layout(hwnd);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, black);
    DeleteObject(black);

    if (g_app->background) {
        const int src_h = min(kBaseHeight, g_app->background.height);
        alpha_blit(dc, g_app->background, g_app->background_rect.left, g_app->background_rect.top, kBaseWidth, kBaseHeight, 0, 0, kBaseWidth, src_h);
    }

    draw_tips(dc);
    draw_sprite(dc, L"window", g_app->panel_rect.left, g_app->panel_rect.top);
    const auto& window = g_app->runtime.connection_window;
    const int title_right = window.title_right > window.title_left
        ? min(window.title_right, window.width - window.title_left)
        : window.width - 10;
    RECT title_rc{
        g_app->panel_rect.left + window.title_left,
        g_app->panel_rect.top + window.title_top,
        g_app->panel_rect.left + title_right,
        g_app->panel_rect.top + (window.title_bottom > window.title_top ? window.title_bottom : 29),
    };
    draw_text_centered(dc, connection_title_text(), title_rc, window.font_index, window.text_color);
    for (const auto& control : g_app->runtime.connection_window.controls) {
        draw_control(dc, control);
    }

    if (!g_app->status.empty()) {
        RECT status_rc{g_app->background_rect.left + 14, g_app->background_rect.bottom - 26, g_app->background_rect.right - 14, g_app->background_rect.bottom - 8};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(220, 220, 220));
        DrawTextW(dc, g_app->status.c_str(), -1, &status_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void paint_game_scene(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, black);
    DeleteObject(black);

    for (const int index : g_app->game_window_z_order) {
        draw_game_window(dc, hwnd, g_app->game_windows[static_cast<std::size_t>(index)], index);
    }
}

void paint_scene_buffered(HWND hwnd, HDC target_dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC mem_dc = CreateCompatibleDC(target_dc);
    HBITMAP back_buffer = CreateCompatibleBitmap(target_dc, width, height);
    if (!mem_dc || !back_buffer) {
        if (back_buffer) {
            DeleteObject(back_buffer);
        }
        if (mem_dc) {
            DeleteDC(mem_dc);
        }
        if (g_app->mode == AppMode::Game) {
            paint_game_scene(hwnd, target_dc);
        } else {
            paint_scene(hwnd, target_dc);
        }
        return;
    }

    HGDIOBJ old_bitmap = SelectObject(mem_dc, back_buffer);
    HGDIOBJ old_font = nullptr;
    if (g_app->ui_font) {
        old_font = SelectObject(mem_dc, g_app->ui_font);
    }

    if (g_app->mode == AppMode::Game) {
        paint_game_scene(hwnd, mem_dc);
    } else {
        paint_scene(hwnd, mem_dc);
    }
    BitBlt(target_dc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);

    if (old_font) {
        SelectObject(mem_dc, old_font);
    }
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(back_buffer);
    DeleteDC(mem_dc);
}

void set_status(HWND hwnd, std::wstring status) {
    g_app->status = std::move(status);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void apply_selected_character_to_scene() {
    if (!g_app->character_scene) {
        return;
    }

    const auto& slot = selected_character_slot();
    if (slot.present) {
        g_app->appearance.gender = slot.female ? 1 : 0;
        g_app->appearance.face = slot.face;
        g_app->appearance.hair = slot.hair;
        g_app->appearance.hair_color = slot.hair_color;
        g_app->appearance.tattoo = slot.tattoo;
    }

    std::wstring error;
    if (!g_app->character_scene->set_character_appearance(
            g_app->appearance.gender != 0,
            g_app->appearance.face,
            g_app->appearance.hair,
            g_app->appearance.hair_color,
            g_app->appearance.tattoo,
            error)) {
        g_app->status = L"Character mesh reload failed: " + error;
    }
}

int first_selectable_character_slot(const std::array<sphere::client::CharacterSlot, 3>& slots) {
    for (int i = 0; i < 3; ++i) {
        if (slots[static_cast<std::size_t>(i)].present) {
            return i;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (slots[static_cast<std::size_t>(i)].can_create) {
            return i;
        }
    }
    return 0;
}

void enter_character_select_3d(HWND hwnd, const sphere::client::LoginProbeResult& result) {
    auto scene = std::make_unique<sphere::client::CharacterSelectScene>();
    scene->set_camera_profiles(g_app->lua_boot.appearance.camera_profiles);
    std::wstring error;
    if (!scene->initialize(hwnd, g_app->settings.root, error)) {
        g_app->login_in_progress = false;
        set_status(hwnd, L"3D scene init failed: " + error);
        return;
    }

    g_app->mode = AppMode::CharacterSelect3D;
    g_app->character_scene = std::move(scene);
    g_app->hot_control_id = 0;
    g_app->pressed_control_id = 0;
    g_app->character_hot_control_id = 0;
    g_app->character_pressed_control_id = 0;
    g_app->character_camera_focus_id = 0;
    g_app->rotating_character = false;
    g_app->character_action_in_progress = false;
    g_app->character_entered_game = false;
    g_app->server_session = result.session;
    g_app->has_game_time = result.has_game_time;
    g_app->game_time_fraction = result.game_time_fraction;
    g_app->character_slots = result.character_slots;
    g_app->selected_character_slot = first_selectable_character_slot(g_app->character_slots);
    for (int i = 0; i < 3; ++i) {
        const auto& slot = g_app->character_slots[static_cast<std::size_t>(i)];
        g_app->character_name_edits[static_cast<std::size_t>(i)] = slot.present ? slot.name : std::wstring{};
    }
    apply_selected_character_to_scene();
    sync_character_select_controls();
    update_character_select_overlay(hwnd);
    start_scene_music(hwnd);
    std::wostringstream status;
    status << L"Character select 3D; localId=0x" << std::hex << result.local_id << std::dec
           << L"; packets=" << result.character_select_packets
           << L" bytes=" << result.character_select_bytes;
    g_app->status = status.str();
    SetWindowTextW(hwnd, L"Sphere NewClient - Character Select");
    SetTimer(hwnd, kRenderTimer, 16, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void open_registration(HWND hwnd);
std::wstring boot_status();
sphere::client::CharacterAppearanceRules network_appearance_rules();

void erase_spaces(std::wstring& text) {
    text.erase(std::remove(text.begin(), text.end(), L' '), text.end());
}

void handle_login(HWND hwnd) {
    if (!g_app->lua_boot.ok || !g_app->lua_boot.appearance.ok) {
        set_status(hwnd, boot_status());
        return;
    }
    erase_spaces(g_app->login);
    if (g_app->login.empty()) {
        open_registration(hwnd);
        return;
    }
    if (g_app->password.empty()) {
        set_status(hwnd, L"Password is empty");
        return;
    }
    if (g_app->login_in_progress) {
        return;
    }
    save_login_file();
    g_app->login_in_progress = true;

    std::wostringstream status;
    status << L"Connecting to " << widen_ascii(g_app->settings.host) << L":" << g_app->settings.port
           << L"...";
    set_status(hwnd, status.str());

    const auto host = g_app->settings.host;
    const int port = g_app->settings.port;
    const auto login = g_app->login;
    const auto password = g_app->password;
    const auto appearance_rules = network_appearance_rules();
    const bool debug_auto_enter = g_app->settings.debug_auto_enter;
    std::thread([hwnd, host, port, login, password, appearance_rules, debug_auto_enter]() {
        auto result = sphere::client::probe_login_server(host, port, login, password, appearance_rules, 2500, debug_auto_enter);
        auto* message = new sphere::client::LoginProbeResult(std::move(result));
        if (!PostMessageW(hwnd, kLoginProbeMessage, 0, reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    }).detach();
}

void open_registration(HWND hwnd) {
    if (g_app->settings.registration_url.empty()) {
        set_status(hwnd, L"Registration URL is absent in connectn.cfg");
        return;
    }
    ShellExecuteA(hwnd, "open", g_app->settings.registration_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void activate_control(HWND hwnd, int id) {
    if (id == 7 || id == 8) {
        g_app->active_edit_id = id;
    } else if (id == 9) {
        g_app->save_password = !g_app->save_password;
        if (!g_app->save_password) {
            save_login_file();
        }
    } else if (id == 1 || id == 4) {
        DestroyWindow(hwnd);
    } else if (id == 3) {
        handle_login(hwnd);
    } else if (id == 11) {
        open_registration(hwnd);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

int wrap_index(int value, int delta, int count) {
    if (count <= 0) {
        return 0;
    }
    value = (value + delta) % count;
    if (value < 0) {
        value += count;
    }
    return value;
}

int character_appearance_option_count(int id) {
    const bool female = g_app->appearance.gender != 0;
    const auto& appearance = g_app->lua_boot.appearance;
    if (!appearance.ok) {
        return 1;
    }
    switch (id) {
    case 13:
        return female ? appearance.female.face_count : appearance.male.face_count;
    case 14:
        return female ? appearance.female.hair_count : appearance.male.hair_count;
    case 15:
        return appearance.hair_color_count;
    case 16:
        return appearance.tattoo_count;
    default:
        return 1;
    }
}

void update_character_appearance_status() {
    const auto& a = g_app->appearance;
    std::wostringstream status;
    status << L"\x0421\x043E\x0437\x0434\x0430\x043D\x0438\x0435 \x043F\x0435\x0440\x0441\x043E\x043D\x0430\x0436\x0430: "
           << (a.gender == 0 ? L"\x043C\x0443\x0436." : L"\x0436\x0435\x043D.")
           << L"; \x043B\x0438\x0446\x043E " << (a.face + 1)
           << L"; \x0432\x043E\x043B\x043E\x0441\x044B " << (a.hair + 1)
           << L"; \x0446\x0432\x0435\x0442 " << (a.hair_color + 1)
           << L"; \x0442\x0430\x0442\x0443 " << (a.tattoo + 1);
    g_app->status = status.str();
}

sphere::client::CharacterCreationAppearance network_appearance() {
    sphere::client::CharacterCreationAppearance out;
    out.female = g_app->appearance.gender != 0;
    out.model_base = g_app->lua_boot.appearance.model_base;
    out.face = g_app->appearance.face;
    out.hair = g_app->appearance.hair;
    out.hair_color = g_app->appearance.hair_color;
    out.tattoo = g_app->appearance.tattoo;
    return out;
}

sphere::client::CharacterAppearanceRules network_appearance_rules() {
    const auto& appearance = g_app->lua_boot.appearance;
    sphere::client::CharacterAppearanceRules out;
    out.model_base = appearance.model_base;
    out.male_face_count = appearance.male.face_count;
    out.female_face_count = appearance.female.face_count;
    out.male_hair_count = appearance.male.hair_count;
    out.female_hair_count = appearance.female.hair_count;
    out.hair_color_count = appearance.hair_color_count;
    out.tattoo_count = appearance.tattoo_count;
    return out;
}

double decode_server_coordinate(const std::vector<std::uint8_t>& frame, std::size_t offset) {
    if (offset + 4 > frame.size()) {
        return 0.0;
    }
    const int scale = frame[offset + 3] & 0x7f;
    if (scale == 58) {
        return 0.0;
    }

    const int sign = (frame[offset + 3] & 0x80) != 0 ? -1 : 1;
    const bool odd_step = (frame[offset + 2] & 0x80) != 0;
    const int encoded = ((frame[offset + 2] & 0x7f) << 16) | (frame[offset + 1] << 8) | frame[offset];
    constexpr double fraction_base = 8388608.0;
    const double mantissa = 1.0 + static_cast<double>(encoded) / fraction_base;

    // Sphere's server-coordinate exponent is derived from half-steps around
    // the [2048, 4096) range. The high bit of byte 2 stores the lost parity.
    int exponent = 0;
    if (scale < 69) {
        const int steps = 2 * (69 - scale) - (odd_step ? 1 : 0);
        exponent = 11 - steps;
    } else {
        const int steps = 2 * (scale - 69) + (odd_step ? 1 : 0);
        exponent = 11 + steps;
    }
    return static_cast<double>(sign) * mantissa * std::pow(2.0, exponent);
}

bool extract_spawn_from_frame(const std::vector<std::uint8_t>& frame, GameWorldState& out) {
    constexpr std::uint8_t marker[] = {0x1a, 0x98, 0x18, 0x19};
    if (frame.size() < 4 + 16) {
        return false;
    }

    for (std::size_t i = 0; i + 4 + 16 <= frame.size(); ++i) {
        if (frame[i] != marker[0] || frame[i + 1] != marker[1] || frame[i + 2] != marker[2] || frame[i + 3] != marker[3]) {
            continue;
        }
        const std::size_t coords = i + 4;
        out.x = decode_server_coordinate(frame, coords);
        // CharacterDbEntrySerializer negates Y and Z for the original client.
        out.y = -decode_server_coordinate(frame, coords + 4);
        out.z = -decode_server_coordinate(frame, coords + 8);
        out.angle = decode_server_coordinate(frame, coords + 12);
        out.has_spawn = true;
        return true;
    }
    return false;
}

GameWorldState game_world_from_action(const PostedCharacterActionResult& posted) {
    GameWorldState world;
    world.selected_slot = std::clamp(posted.slot, 0, 2);
    world.select_packet_count = posted.result.packet_count;
    world.select_byte_count = posted.result.byte_count;
    world.world_packet_count = posted.ack_result.packet_count;
    world.world_byte_count = posted.ack_result.byte_count;

    if (!posted.name.empty()) {
        world.character_name = posted.name;
    } else {
        const auto& slot = g_app->character_slots[static_cast<std::size_t>(world.selected_slot)];
        world.character_name = slot.name;
    }

    for (const auto& frame : posted.result.frames) {
        if (extract_spawn_from_frame(frame, world)) {
            return world;
        }
    }
    return world;
}

std::wstring normalized_character_name(std::wstring value) {
    value = trim(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }), value.end());
    if (value.size() > kMaxCharacterNameChars) {
        value.resize(kMaxCharacterNameChars);
    }
    return value;
}

void select_character_slot(HWND hwnd, int slot) {
    if (g_app->character_action_in_progress || g_app->character_entered_game) {
        return;
    }
    g_app->selected_character_slot = std::clamp(slot, 0, 2);
    apply_selected_character_to_scene();
    sync_character_select_controls();
    update_character_select_overlay(hwnd);
}

void post_character_action(HWND hwnd, std::unique_ptr<PostedCharacterActionResult> message) {
    if (!PostMessageW(hwnd, kCharacterActionMessage, 0, reinterpret_cast<LPARAM>(message.get()))) {
        return;
    }
    message.release();
}

bool enter_game_mode(HWND hwnd, const PostedCharacterActionResult& posted) {
    auto world = game_world_from_action(posted);
    if (!world.has_spawn) {
        append_client_log(L"enter_game_mode: character game data packet does not contain spawn coordinates");
        set_status(hwnd, L"Character game data packet does not contain spawn coordinates");
        return false;
    }

    if (!g_app->lua_boot.game_window.ok) {
        append_client_log(L"enter_game_mode: game world Lua configuration is invalid");
        set_status(hwnd, L"Game world Lua configuration is invalid");
        return false;
    }
    auto scene = std::make_unique<sphere::client::GameWorldScene>();
    const auto player_mesh = g_app->character_scene
        ? g_app->character_scene->character_render_mesh()
        : sphere::client::CharacterRenderMesh{};
    if (!player_mesh.valid()) {
        append_client_log(L"enter_game_mode: selected character render mesh is unavailable");
        set_status(hwnd, L"Selected character render mesh is unavailable");
        return false;
    }
    std::wstring error;
    auto world_config = g_app->lua_boot.game_window;
    world_config.grass_quality = g_app->settings.grass_quality;
    if (!scene->initialize(
            hwnd,
            g_app->settings.root,
            world_config,
            &player_mesh,
            world.x,
            -world.y,
            -world.z,
            world.angle,
            error)) {
        append_client_log(L"enter_game_mode: Game world init failed: " + error);
        set_status(hwnd, L"Game world init failed: " + error);
        return false;
    }
    if (g_app->has_game_time) {
        scene->set_game_time(g_app->game_time_fraction);
    }

    g_app->game_world = std::move(world);
    g_app->mode = AppMode::Game;
    g_app->game_scene = std::move(scene);
    g_app->character_entered_game = true;
    g_app->rotating_character = false;
    g_app->character_pressed_control_id = 0;
    g_app->character_hot_control_id = 0;
    g_app->character_scene.reset();
    g_app->game_movement = {};
    g_app->game_movement.run = g_app->settings.always_run;
    g_app->game_last_tick = GetTickCount64();
    g_app->game_last_position_send_tick = 0;
    g_app->game_last_overlay_tick = 0;

    std::wostringstream status;
    status << L"Entered game; " << format_game_spawn_line().substr(2);
    g_app->status = status.str();
    append_client_log(g_app->status);
    update_game_overlay(hwnd);
    SetWindowTextW(hwnd, L"Sphere NewClient - Game");
    SetTimer(hwnd, kRenderTimer, 16, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

bool enter_debug_game_mode(HWND hwnd) {
    auto scene = std::make_unique<sphere::client::GameWorldScene>();
    std::wstring error;
    auto world_config = g_app->lua_boot.game_window;
    world_config.grass_quality = g_app->settings.grass_quality;
    if (!scene->initialize(
            hwnd,
            g_app->settings.root,
            world_config,
            nullptr,
            g_app->settings.debug_game_x,
            g_app->settings.debug_game_y,
            g_app->settings.debug_game_z,
            g_app->settings.debug_game_angle,
            error)) {
        append_client_log(L"enter_debug_game_mode: " + error);
        set_status(hwnd, L"Game world init failed: " + error);
        return false;
    }
    g_app->game_world.has_spawn = true;
    g_app->game_world.x = g_app->settings.debug_game_x;
    g_app->game_world.y = g_app->settings.debug_game_y;
    g_app->game_world.z = g_app->settings.debug_game_z;
    g_app->game_world.angle = g_app->settings.debug_game_angle;
    g_app->mode = AppMode::Game;
    g_app->game_scene = std::move(scene);
    g_app->game_movement = {};
    g_app->game_movement.run = g_app->settings.always_run;
    g_app->game_last_tick = GetTickCount64();
    update_game_overlay(hwnd);
    SetWindowTextW(hwnd, L"Sphere NewClient - Game");
    SetTimer(hwnd, kRenderTimer, 16, nullptr);
    return true;
}

void start_character_action(HWND hwnd, CharacterActionKind kind) {
    if (g_app->character_action_in_progress) {
        return;
    }
    if (g_app->character_entered_game) {
        set_status(hwnd, L"Character data already received; waiting for game scene integration");
        return;
    }
    if (!g_app->server_session || !g_app->server_session->connected()) {
        set_status(hwnd, L"Character server session is closed");
        return;
    }

    const int slot = selected_slot_index();
    const auto& slot_state = g_app->character_slots[static_cast<std::size_t>(slot)];
    auto name = normalized_character_name(g_app->character_name_edits[static_cast<std::size_t>(slot)]);
    if (kind == CharacterActionKind::Create) {
        if (slot_state.present || !slot_state.can_create) {
            set_status(hwnd, L"Selected slot cannot create a character");
            return;
        }
        if (name.size() < 3) {
            set_status(hwnd, L"Character name is too short");
            return;
        }
        g_app->character_name_edits[static_cast<std::size_t>(slot)] = name;
    }
    if (kind == CharacterActionKind::Select && !slot_state.present) {
        set_status(hwnd, L"Selected slot is empty");
        return;
    }
    if (kind == CharacterActionKind::Delete && !slot_state.present) {
        set_status(hwnd, L"Selected slot is empty");
        return;
    }

    g_app->character_action_in_progress = true;
    sync_character_select_controls();
    update_character_select_overlay(hwnd);

    const auto session = g_app->server_session;
    const auto appearance = network_appearance();
    std::thread([hwnd, session, kind, slot, name, appearance]() {
        auto posted = std::make_unique<PostedCharacterActionResult>();
        posted->kind = kind;
        posted->slot = slot;
        posted->name = name;
        posted->appearance = appearance;
        switch (kind) {
        case CharacterActionKind::Select:
            posted->result = session->select_character(slot);
            if (posted->result.ok) {
                posted->ack_result = session->send_ingame_ack(1000);
            }
            break;
        case CharacterActionKind::Create:
            posted->result = session->create_character(slot, name, appearance);
            if (posted->result.ok) {
                posted->ack_result = session->send_ingame_ack(1000);
            }
            break;
        case CharacterActionKind::Delete:
            posted->result = session->delete_character(slot);
            break;
        case CharacterActionKind::Ack:
            posted->result = session->send_ingame_ack(1000);
            break;
        }
        post_character_action(hwnd, std::move(posted));
    }).detach();
}

void apply_character_action_result(HWND hwnd, const PostedCharacterActionResult& posted) {
    g_app->character_action_in_progress = false;
    const int slot = std::clamp(posted.slot, 0, 2);
    auto& slot_state = g_app->character_slots[static_cast<std::size_t>(slot)];
    bool should_enter_game = false;

    if (posted.result.ok && posted.kind == CharacterActionKind::Create) {
        slot_state.present = true;
        slot_state.can_create = true;
        slot_state.name = posted.name;
        slot_state.female = posted.appearance.female;
        slot_state.face = posted.appearance.face;
        slot_state.hair = posted.appearance.hair;
        slot_state.hair_color = posted.appearance.hair_color;
        slot_state.tattoo = posted.appearance.tattoo;
        slot_state.strength = g_app->appearance.strength;
        slot_state.dexterity = g_app->appearance.dexterity;
        slot_state.accuracy = g_app->appearance.accuracy;
        slot_state.endurance = g_app->appearance.endurance;
        slot_state.fire = g_app->appearance.fire;
        slot_state.water = g_app->appearance.water;
        slot_state.earth = g_app->appearance.earth;
        slot_state.air = g_app->appearance.air;
        apply_selected_character_to_scene();
        should_enter_game = true;
    } else if (posted.result.ok && posted.kind == CharacterActionKind::Select) {
        should_enter_game = true;
    } else if (posted.result.ok && posted.kind == CharacterActionKind::Delete) {
        slot_state = sphere::client::CharacterSlot{};
        slot_state.slot = slot;
        g_app->server_session.reset();
        g_app->character_entered_game = true;
    }

    std::wostringstream status;
    status << widen_ascii(posted.result.message);
    if (posted.ack_result.ok) {
        status << L"; " << widen_ascii(posted.ack_result.message);
    }
    if (!posted.result.ok) {
        status << L" (failed)";
    }
    g_app->status = status.str();

    if (should_enter_game && enter_game_mode(hwnd, posted)) {
        return;
    }

    sync_character_select_controls();
    update_character_select_overlay(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool cycle_character_setting(HWND hwnd, int id) {
    if (id < 12 || id > 16) {
        return false;
    }
    const auto& slot = selected_character_slot();
    if (slot.present || !slot.can_create || g_app->character_action_in_progress) {
        return false;
    }

    auto& a = g_app->appearance;
    const int delta = g_app->character_spin_delta < 0 ? -1 : 1;
    switch (id) {
    case 12: {
        a.gender = wrap_index(a.gender, delta, 2);
        a.face = 0;
        a.hair = 0;
        a.hair_color = 0;
        a.tattoo = 0;
        break;
    }
    case 13:
        a.face = wrap_index(a.face, delta, character_appearance_option_count(id));
        break;
    case 14:
        a.hair = wrap_index(a.hair, delta, character_appearance_option_count(id));
        break;
    case 15:
        a.hair_color = wrap_index(a.hair_color, delta, character_appearance_option_count(id));
        break;
    case 16:
        a.tattoo = wrap_index(a.tattoo, delta, character_appearance_option_count(id));
        break;
    default:
        return false;
    }

    if (g_app->character_scene) {
        std::wstring error;
        if (!g_app->character_scene->set_character_appearance(
                a.gender != 0,
                a.face,
                a.hair,
                a.hair_color,
                a.tattoo,
                error)) {
            g_app->status = L"Character mesh reload failed: " + error;
            return true;
        }
    }
    update_character_appearance_status();
    return true;
}

void activate_character_control(HWND hwnd, int id) {
    if (id >= 63 && id <= 65) {
        select_character_slot(hwnd, id - 63);
    } else if (id >= 60 && id <= 62) {
        g_app->active_character_edit_id = id;
    } else if (const int focus_id = character_focus_for_control(id); focus_id != 0) {
        set_character_camera_focus(focus_id);
        cycle_character_setting(hwnd, id);
    } else if (id == 58) {
        DestroyWindow(hwnd);
    } else if (id == 57) {
        start_character_action(hwnd, CharacterActionKind::Delete);
    } else if (id == 59) {
        const auto& slot = selected_character_slot();
        start_character_action(hwnd, slot.present ? CharacterActionKind::Select : CharacterActionKind::Create);
    }
    update_character_select_overlay(hwnd);
}

void append_char(wchar_t ch) {
    auto& target = g_app->active_edit_id == 8 ? g_app->password : g_app->login;
    const std::size_t limit = g_app->active_edit_id == 8 ? kMaxPasswordChars : kMaxLoginChars;
    if (ch >= 32 && target.size() < limit) {
        target.push_back(ch);
    }
}

void backspace_char() {
    auto& target = g_app->active_edit_id == 8 ? g_app->password : g_app->login;
    if (!target.empty()) {
        target.pop_back();
    }
}

void append_character_name_char(wchar_t ch) {
    const int edit_id = g_app->active_character_edit_id;
    if (edit_id < 60 || edit_id > 62 || g_app->character_action_in_progress || g_app->character_entered_game) {
        return;
    }
    const int slot = edit_id - 60;
    auto& target = g_app->character_name_edits[static_cast<std::size_t>(slot)];
    if (std::iswspace(ch) || ch < 32 || target.size() >= kMaxCharacterNameChars) {
        return;
    }
    target.push_back(ch);
}

void backspace_character_name_char() {
    const int edit_id = g_app->active_character_edit_id;
    if (edit_id < 60 || edit_id > 62 || g_app->character_action_in_progress || g_app->character_entered_game) {
        return;
    }
    const int slot = edit_id - 60;
    auto& target = g_app->character_name_edits[static_cast<std::size_t>(slot)];
    if (!target.empty()) {
        target.pop_back();
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_app->ui_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_app->mode == AppMode::CharacterSelect3D && g_app->character_scene) {
            g_app->character_scene->resize();
            return 0;
        }
        if (g_app->mode == AppMode::Game && g_app->game_scene) {
            g_app->game_scene->resize();
            if (g_app->game_look_mode) {
                g_app->game_mouse_center = POINT{(LOWORD(lparam)) / 2, (HIWORD(lparam)) / 2};
            }
            update_game_overlay(hwnd);
            return 0;
        }
        update_layout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g_app->mode == AppMode::CharacterSelect3D) {
            if (g_app->ui_drag_kind == UiDragKind::CharacterSelect) {
                const int dx = pt.x - g_app->ui_drag_last_mouse.x;
                const int dy = pt.y - g_app->ui_drag_last_mouse.y;
                g_app->ui_drag_last_mouse = pt;
                move_ui_window(hwnd, g_app->pick_person_window, dx, dy);
                g_app->character_overlay_dirty = true;
                return 0;
            }
            if (g_app->rotating_character && g_app->character_scene) {
                const int dx = pt.x - g_app->last_character_mouse.x;
                g_app->last_character_mouse = pt;
                g_app->character_scene->rotate_character(static_cast<float>(dx) * 0.01f);
                return 0;
            }
            int hot = character_hit_test(hwnd, pt, true);
            if (hot < 0) {
                hot = 0;
            }
            if (hot != g_app->character_hot_control_id) {
                g_app->character_hot_control_id = hot;
                update_character_select_overlay(hwnd);
            }
            return 0;
        }
        if (g_app->mode == AppMode::Game) {
            g_app->game_pressed_mouse = pt;
            if (g_app->ui_drag_kind == UiDragKind::Game) {
                const int index = g_app->ui_drag_game_window_index;
                if (index >= 0 && index < static_cast<int>(g_app->game_windows.size())) {
                    const int dx = pt.x - g_app->ui_drag_last_mouse.x;
                    const int dy = pt.y - g_app->ui_drag_last_mouse.y;
                    g_app->ui_drag_last_mouse = pt;
                    move_ui_window(hwnd, g_app->game_windows[static_cast<std::size_t>(index)], dx, dy);
                    g_app->game_overlay_dirty = true;
                }
                return 0;
            }
            if (g_app->game_look_mode && g_app->game_scene) {
                const int dx = pt.x - g_app->game_mouse_center.x;
                const int dy = pt.y - g_app->game_mouse_center.y;
                if (dx != 0 || dy != 0) {
                    g_app->game_scene->rotate_view(static_cast<float>(dx), static_cast<float>(dy));
                    POINT screen = g_app->game_mouse_center;
                    ClientToScreen(hwnd, &screen);
                    SetCursorPos(screen.x, screen.y);
                }
                return 0;
            }
            const auto [hot_window, hot_control] = game_hit_test(hwnd, pt);
            if (hot_window != g_app->game_hot_window_index || hot_control != g_app->game_hot_control_id) {
                g_app->game_hot_window_index = hot_window;
                g_app->game_hot_control_id = hot_control;
                update_game_overlay(hwnd);
            }
            return 0;
        }
        if (g_app->mode != AppMode::Login) {
            return 0;
        }
        const int hot = hit_test(pt, true);
        if (hot != g_app->hot_control_id) {
            const int old_hot = g_app->hot_control_id;
            g_app->hot_control_id = hot;
            invalidate_controls(hwnd, old_hot, hot);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g_app->mode == AppMode::CharacterSelect3D) {
            g_app->character_pressed_control_id = character_hit_test(hwnd, pt, false);
            g_app->character_spin_delta = character_spin_delta_for_point(hwnd, g_app->character_pressed_control_id, pt);
            if (g_app->character_pressed_control_id != 0) {
                SetCapture(hwnd);
                if (const int focus_id = character_focus_for_control(g_app->character_pressed_control_id); focus_id != 0) {
                    set_character_camera_focus(focus_id);
                }
                update_character_select_overlay(hwnd);
            } else if (ui_window_title_hit(hwnd, g_app->pick_person_window, pt)) {
                begin_ui_drag(hwnd, g_app->pick_person_window, UiDragKind::CharacterSelect, -1, pt);
                update_character_select_overlay(hwnd);
            } else {
                const RECT panel = ui_window_rect(hwnd, g_app->pick_person_window);
                if (!PtInRect(&panel, pt)) {
                    SetCapture(hwnd);
                    g_app->rotating_character = true;
                    g_app->last_character_mouse = pt;
                }
            }
            return 0;
        }
        if (g_app->mode == AppMode::Game) {
            const auto [pressed_window, pressed_control] = game_hit_test(hwnd, pt);
            g_app->game_pressed_window_index = pressed_window;
            g_app->game_pressed_control_id = pressed_control;
            g_app->game_pressed_mouse = pt;
            if (pressed_control != 0) {
                bring_game_window_to_front(pressed_window);
                SetCapture(hwnd);
            } else {
                for (auto order = g_app->game_window_z_order.rbegin(); order != g_app->game_window_z_order.rend(); ++order) {
                    const int index = *order;
                    if (index < static_cast<int>(g_app->game_window_visible.size()) &&
                        !g_app->game_window_visible[static_cast<std::size_t>(index)]) {
                        continue;
                    }
                    auto& window = g_app->game_windows[static_cast<std::size_t>(index)];
                    const RECT window_rect = ui_window_rect(hwnd, window);
                    if (ui_window_title_hit(hwnd, window, pt) ||
                        (window.can_drag_drop && PtInRect(&window_rect, pt))) {
                        bring_game_window_to_front(index);
                        begin_ui_drag(hwnd, window, UiDragKind::Game, index, pt);
                        break;
                    }
                }
            }
            update_game_overlay(hwnd);
            return 0;
        }
        if (g_app->mode != AppMode::Login) {
            return 0;
        }
        SetCapture(hwnd);
        g_app->pressed_control_id = hit_test(pt, false);
        invalidate_control(hwnd, g_app->pressed_control_id);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g_app->mode == AppMode::CharacterSelect3D) {
            const bool was_dragging = g_app->ui_drag_kind == UiDragKind::CharacterSelect;
            const int released = character_hit_test(hwnd, pt, false);
            const int pressed = g_app->character_pressed_control_id;
            ReleaseCapture();
            end_ui_drag();
            g_app->rotating_character = false;
            g_app->character_pressed_control_id = 0;
            if (!was_dragging && pressed != 0 && pressed == released) {
                activate_character_control(hwnd, released);
            } else {
                update_character_select_overlay(hwnd);
            }
            return 0;
        }
        if (g_app->mode == AppMode::Game) {
            const auto [released_window, released_control] = game_hit_test(hwnd, pt);
            const int pressed_window = g_app->game_pressed_window_index;
            const int pressed_control = g_app->game_pressed_control_id;
            ReleaseCapture();
            end_ui_drag();
            g_app->game_pressed_window_index = -1;
            g_app->game_pressed_control_id = 0;
            if (pressed_control != 0 && pressed_window == released_window && pressed_control == released_control) {
                activate_game_control(hwnd, released_window, released_control, pt);
            }
            update_game_overlay(hwnd);
            return 0;
        }
        if (g_app->mode != AppMode::Login) {
            return 0;
        }
        ReleaseCapture();
        const int released = hit_test(pt, false);
        const int pressed = g_app->pressed_control_id;
        g_app->pressed_control_id = 0;
        if (pressed != 0 && pressed == released) {
            activate_control(hwnd, released);
        }
        invalidate_controls(hwnd, pressed, released);
        return 0;
    }
    case WM_CAPTURECHANGED:
        end_ui_drag();
        if (g_app->mode == AppMode::CharacterSelect3D) {
            g_app->rotating_character = false;
            g_app->character_pressed_control_id = 0;
            update_character_select_overlay(hwnd);
        } else if (g_app->mode == AppMode::Game) {
            g_app->game_pressed_window_index = -1;
            g_app->game_pressed_control_id = 0;
            if (g_app->game_look_mode) {
                set_game_look_mode(hwnd, false);
            }
            update_game_overlay(hwnd);
        }
        return 0;
    case WM_KEYDOWN:
        if (g_app->mode == AppMode::CharacterSelect3D) {
            if (wparam == VK_ESCAPE) {
                DestroyWindow(hwnd);
            } else if (wparam == VK_RETURN) {
                activate_character_control(hwnd, 59);
            } else if (wparam == VK_BACK) {
                backspace_character_name_char();
                update_character_select_overlay(hwnd);
            } else if (wparam == VK_TAB) {
                select_character_slot(hwnd, (selected_slot_index() + 1) % 3);
            }
            return 0;
        }
        if (g_app->mode == AppMode::Game) {
            if (g_app->game_active_edit_window_index >= 0) {
                if (wparam == VK_ESCAPE) {
                    g_app->game_active_edit_window_index = -1;
                    g_app->game_active_edit_control_id = 0;
                    g_app->game_edit_text.clear();
                } else if (wparam == VK_BACK) {
                    if (!g_app->game_edit_text.empty()) {
                        g_app->game_edit_text.pop_back();
                    }
                } else if (wparam == VK_RETURN) {
                    const auto& edit_window = g_app->game_windows[static_cast<std::size_t>(g_app->game_active_edit_window_index)];
                    if (lowercase(edit_window.name) == L"chat_st2" && !g_app->game_edit_text.empty()) {
                        const auto author = g_app->game_world.character_name.empty() ? L"Player" : g_app->game_world.character_name;
                        g_app->game_chat_lines.push_back(author + L": " + g_app->game_edit_text);
                        if (g_app->game_chat_lines.size() > 256) {
                            g_app->game_chat_lines.erase(g_app->game_chat_lines.begin());
                        }
                        g_app->game_edit_text.clear();
                    }
                }
                update_game_overlay(hwnd);
                return 0;
            }
            if (set_game_movement_key(wparam, lparam, true)) {
                return 0;
            }
            if (static_cast<int>(wparam) == g_app->settings.key_cursor) {
                set_game_look_mode(hwnd, !g_app->game_look_mode);
            } else if (static_cast<int>(wparam) == g_app->settings.key_run && (lparam & (1LL << 30)) == 0) {
                g_app->game_movement.run = !g_app->game_movement.run;
            } else if (wparam == VK_ESCAPE && g_app->game_look_mode) {
                set_game_look_mode(hwnd, false);
            } else if (wparam == VK_ESCAPE) {
                if (!close_topmost_game_window()) {
                    toggle_game_options(hwnd);
                } else {
                    update_game_overlay(hwnd);
                }
            }
            return 0;
        }
        if (wparam == VK_TAB) {
            g_app->active_edit_id = g_app->active_edit_id == 7 ? 8 : 7;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wparam == VK_RETURN) {
            handle_login(hwnd);
            return 0;
        }
        if (wparam == VK_BACK) {
            backspace_char();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    case WM_KEYUP:
        if (g_app->mode == AppMode::Game) {
            set_game_movement_key(wparam, lparam, false);
            return 0;
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (!wparam && g_app->mode == AppMode::Game) {
            g_app->game_movement.forward = false;
            g_app->game_movement.backward = false;
            g_app->game_movement.strafe_left = false;
            g_app->game_movement.strafe_right = false;
            if (g_app->game_look_mode) {
                set_game_look_mode(hwnd, false);
            }
        }
        return 0;
    case WM_CHAR:
        if (g_app->mode == AppMode::CharacterSelect3D) {
            if (wparam >= 32) {
                append_character_name_char(static_cast<wchar_t>(wparam));
                update_character_select_overlay(hwnd);
            }
            return 0;
        }
        if (g_app->mode == AppMode::Game) {
            if (g_app->game_active_edit_window_index >= 0 && wparam >= 32 && g_app->game_edit_text.size() < 256) {
                g_app->game_edit_text.push_back(static_cast<wchar_t>(wparam));
                update_game_overlay(hwnd);
            }
            return 0;
        }
        if (g_app->mode != AppMode::Login) {
            return 0;
        }
        if (wparam >= 32) {
            append_char(static_cast<wchar_t>(wparam));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_app->mode == AppMode::CharacterSelect3D && g_app->character_scene) {
            EndPaint(hwnd, &ps);
            render_character_select_frame(hwnd);
            return 0;
        }
        if (g_app->mode == AppMode::Game && g_app->game_scene) {
            EndPaint(hwnd, &ps);
            render_game_frame(hwnd);
            return 0;
        }
        paint_scene_buffered(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kRenderTimer && g_app->mode == AppMode::CharacterSelect3D && g_app->character_scene) {
            if (g_app->character_overlay_dirty) {
                g_app->character_overlay_dirty = false;
                update_character_select_overlay(hwnd);
            }
            render_character_select_frame(hwnd);
            return 0;
        }
        if (wparam == kRenderTimer && g_app->mode == AppMode::Game && g_app->game_scene) {
            if (g_app->game_overlay_dirty) {
                g_app->game_overlay_dirty = false;
                update_game_overlay(hwnd);
            }
            update_game_frame(hwnd);
            return 0;
        }
        return 0;
    case kLoginProbeMessage: {
        std::unique_ptr<sphere::client::LoginProbeResult> result(reinterpret_cast<sphere::client::LoginProbeResult*>(lparam));
        g_app->login_in_progress = false;
        if (result && result->character_select_ready) {
            enter_character_select_3d(hwnd, *result);
            return 0;
        }
        set_status(hwnd, result ? widen_ascii(result->message) : L"Login probe failed");
        return 0;
    }
    case kCharacterActionMessage: {
        std::unique_ptr<PostedCharacterActionResult> result(reinterpret_cast<PostedCharacterActionResult*>(lparam));
        if (result) {
            apply_character_action_result(hwnd, *result);
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kRenderTimer);
        if (g_app && g_app->game_look_mode) {
            set_game_look_mode(hwnd, false);
        }
        stop_scene_music();
        if (g_app) {
            g_app->character_scene.reset();
            g_app->game_scene.reset();
            g_app->server_session.reset();
        }
        if (g_app && g_app->ui_font) {
            DeleteObject(g_app->ui_font);
            g_app->ui_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

std::wstring boot_status() {
    if (!g_app->runtime.use_lua_scripts) {
        return L"Lua script manifest is missing or invalid";
    }
    if (!g_app->lua_boot.ok) {
        return L"Lua init failed: " + g_app->lua_boot.message;
    }
    return {};
}

int run(HINSTANCE instance, int show_command) {
    auto state = std::make_unique<AppState>();
    state->settings = load_settings();
    load_saved_login(*state);
    state->runtime = sphere::client::load_client_runtime(state->settings.root, state->settings.lang, state->settings.connect_type);
    state->lua_runtime = std::make_unique<sphere::client::LuaRuntime>();
    state->lua_boot = state->lua_runtime->initialize(state->settings.root);
    state->pick_person_window = sphere::client::load_ui_window(state->settings.root / "effects" / "pickpers.ui");
    if (state->lua_boot.ok) {
        state->game_windows = load_ui_windows(state->settings.root, state->lua_boot.game_window.ui_windows);
        state->settings_window_start_index = static_cast<int>(state->game_windows.size());
        auto settings_windows = load_ui_windows(state->settings.root, state->lua_boot.game_window.settings_windows);
        state->game_windows.insert(
            state->game_windows.end(),
            std::make_move_iterator(settings_windows.begin()),
            std::make_move_iterator(settings_windows.end()));
        state->game_window_visible.assign(state->game_windows.size(), false);
        state->game_window_z_order.reserve(state->game_windows.size());
        for (int index = 0; index < static_cast<int>(state->game_windows.size()); ++index) {
            state->game_window_z_order.push_back(index);
        }
        const auto window_index = [&](const std::wstring& name) {
            const auto expected = lowercase(name);
            for (std::size_t index = 0; index < state->game_windows.size(); ++index) {
                if (lowercase(state->game_windows[index].name) == expected) {
                    return static_cast<int>(index);
                }
            }
            return -1;
        };
        for (const auto& name : state->lua_boot.game_window.ui_initially_visible) {
            const int index = window_index(name);
            if (index < 0 || index >= state->settings_window_start_index) {
                throw std::runtime_error("Lua initial UI window is not loaded");
            }
            state->game_window_visible[static_cast<std::size_t>(index)] = true;
        }
        for (const auto& action : state->lua_boot.game_window.ui_actions) {
            const int source = window_index(action.window);
            if (source < 0 || source >= state->settings_window_start_index) {
                throw std::runtime_error("Lua UI action source window is not loaded");
            }
            const auto& controls = state->game_windows[static_cast<std::size_t>(source)].controls;
            if (std::none_of(controls.begin(), controls.end(), [&](const auto& value) {
                    return value.id == action.control;
                })) {
                throw std::runtime_error("Lua UI action source control is not loaded");
            }
            const auto kind = lowercase(action.action);
            const bool needs_target =
                kind == L"toggle_window" || kind == L"show_window" ||
                kind == L"hide_window" || kind == L"swap_window" ||
                kind == L"cycle_pair";
            if (needs_target && window_index(action.target) < 0) {
                throw std::runtime_error("Lua UI action target window is not loaded");
            }
            if (kind == L"cycle_pair" && window_index(action.alternate) < 0) {
                throw std::runtime_error("Lua UI action alternate window is not loaded");
            }
            if (!needs_target && kind != L"toggle_chat") {
                throw std::runtime_error("Lua UI action type is not supported");
            }
        }
        for (const auto& checked : state->lua_boot.game_window.ui_initially_checked) {
            const int index = window_index(checked.window);
            if (index < 0) {
                throw std::runtime_error("Lua checked UI control window is not loaded");
            }
            const auto& controls = state->game_windows[static_cast<std::size_t>(index)].controls;
            const auto control = std::find_if(controls.begin(), controls.end(), [&](const auto& value) {
                return value.id == checked.control;
            });
            if (control == controls.end()) {
                throw std::runtime_error("Lua checked UI control is not loaded");
            }
            const auto key =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(index)) << 32) |
                static_cast<std::uint32_t>(checked.control);
            state->game_control_checked[key] = true;
        }
    }
    apply_login_script_bootstrap(state->runtime.connection_window);
    state->background = sphere::client::load_dds_rgb_bitmap(state->runtime.login_background_path);
    state->fonts = load_fonts(state->settings.root);
    state->tip = load_tip_of_day(state->settings.root, state->settings.lang);
    g_app = std::move(state);
    preload_connection_textures();
    g_app->status = boot_status();

    const wchar_t* class_name = L"SphereNewClientWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = class_name;

    HICON icon = static_cast<HICON>(LoadImageW(nullptr, (g_app->settings.root / "MainIcon.ico").c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
    wc.hIcon = icon;
    wc.hIconSm = icon;

    if (!RegisterClassExW(&wc)) {
        throw std::runtime_error("RegisterClassExW failed");
    }

    RECT rect{0, 0, g_app->settings.width, g_app->settings.height};
    const DWORD style = g_app->settings.windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExW(0, class_name, make_title(g_app->settings).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        throw std::runtime_error("CreateWindowExW failed");
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);
    if (g_app->settings.debug_start_character_scene) {
        sphere::client::LoginProbeResult fake_result;
        fake_result.local_id = 0;
        enter_character_select_3d(hwnd, fake_result);
    } else if (g_app->settings.debug_start_game_world) {
        enter_debug_game_mode(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (icon) {
        DestroyIcon(icon);
    }
    g_app.reset();
    return static_cast<int>(msg.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    try {
        return run(instance, show_command);
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "Sphere NewClient startup error", MB_ICONERROR | MB_OK);
        return 1;
    }
}
