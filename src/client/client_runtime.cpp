#include "client/client_runtime.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace sphere::client {
namespace {

std::wstring wide_name(const std::filesystem::path& path) {
    return path.wstring();
}

void add_check(std::vector<ResourceCheck>& checks, const std::wstring& name, const std::filesystem::path& path) {
    checks.push_back(ResourceCheck{name, std::filesystem::exists(path)});
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0, std::ios::beg);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

std::optional<int> extract_manifest_int(const std::string& text, const char* key) {
    const auto key_pos = text.find(key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    const auto colon_pos = text.find(':', key_pos + std::strlen(key));
    if (colon_pos == std::string::npos) {
        return std::nullopt;
    }

    std::size_t pos = colon_pos + 1;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
        return std::nullopt;
    }

    int value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return value;
}

int sum_manifest_ints(const std::string& text, const char* key) {
    int total = 0;
    std::size_t cursor = 0;
    while (true) {
        const auto key_pos = text.find(key, cursor);
        if (key_pos == std::string::npos) {
            break;
        }
        const auto colon_pos = text.find(':', key_pos + std::strlen(key));
        if (colon_pos == std::string::npos) {
            break;
        }

        std::size_t pos = colon_pos + 1;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
        }
        int value = 0;
        bool has_digits = false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
            value = value * 10 + (text[pos] - '0');
            has_digits = true;
            ++pos;
        }
        if (has_digits) {
            total += value;
        }
        cursor = pos;
    }
    return total;
}

LuaProjectSummary load_lua_summary(const std::filesystem::path& lua_dir) {
    LuaProjectSummary summary;
    const auto manifest_path = lua_dir / "manifest.json";
    const auto manifest = read_text_file(manifest_path);
    if (!manifest) {
        return summary;
    }

    summary.manifest_present = true;
    summary.scripts = extract_manifest_int(*manifest, "\"scripts_generated\"").value_or(0);
    summary.failed_scripts = extract_manifest_int(*manifest, "\"scripts_failed\"").value_or(0);
    summary.functions = sum_manifest_ints(*manifest, "\"functions\"");
    summary.programs = sum_manifest_ints(*manifest, "\"programs\"");
    return summary;
}

} // namespace

std::filesystem::path strings_path_for_lang(const std::filesystem::path& root, int lang) {
    switch (lang) {
    case 1:
        return root / "language" / "strings_e.ui";
    case 2:
        return root / "language" / "strings_i.ui";
    case 3:
        return root / "language" / "strings_p.ui";
    default:
        return root / "language" / "strings.ui";
    }
}

std::filesystem::path login_background_for_lang(const std::filesystem::path& root, int lang, int connect_type) {
    const bool english = lang == 1;
    const bool sphere_one = connect_type == 1;
    if (english) {
        return root / "xadd" / (sphere_one ? "login_eng_s1.dds" : "login_eng_sp.dds");
    }
    return root / "xadd" / (sphere_one ? "login_rus_s1.dds" : "login_rus_sp.dds");
}

ClientRuntime load_client_runtime(const std::filesystem::path& root, int lang, int connect_type) {
    ClientRuntime runtime;
    runtime.strings_path = strings_path_for_lang(root, lang);
    runtime.connection_ui_path = root / "effects" / "connection.ui";
    runtime.login_background_path = login_background_for_lang(root, lang, connect_type);

    runtime.strings = load_ui_strings(runtime.strings_path);
    runtime.connection_window = load_ui_window(runtime.connection_ui_path);
    runtime.lua = load_lua_summary(root / "lua");
    runtime.use_lua_scripts = runtime.lua.manifest_present && runtime.lua.scripts > 0 && runtime.lua.failed_scripts == 0;

    add_check(runtime.resource_checks, L"config.cfg", root / "config.cfg");
    add_check(runtime.resource_checks, L"connect.cfg", root / "connect.cfg");
    add_check(runtime.resource_checks, L"effects\\connection.ui", runtime.connection_ui_path);
    add_check(runtime.resource_checks, L"effects\\loadscreen.ui", root / "effects" / "loadscreen.ui");
    add_check(runtime.resource_checks, L"language strings", runtime.strings_path);
    add_check(runtime.resource_checks, L"login DDS", runtime.login_background_path);
    add_check(runtime.resource_checks, L"lua\\manifest.json", root / "lua" / "manifest.json");
    add_check(runtime.resource_checks, L"lua\\_main.lua", root / "lua" / "_main.lua");
    add_check(runtime.resource_checks, L"params", root / "params");
    add_check(runtime.resource_checks, L"textures", root / "textures");
    add_check(runtime.resource_checks, L"xadd", root / "xadd");
    return runtime;
}

} // namespace sphere::client
