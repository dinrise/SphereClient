#pragma once

#include "client/ui_definition.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace sphere::client {

struct LuaProjectSummary {
    bool manifest_present = false;
    int scripts = 0;
    int failed_scripts = 0;
    int programs = 0;
    int functions = 0;
};

struct ResourceCheck {
    std::wstring name;
    bool ok = false;
};

struct ClientRuntime {
    UiStringTable strings;
    UiWindowDef connection_window;
    LuaProjectSummary lua;
    bool use_lua_scripts = false;
    std::filesystem::path strings_path;
    std::filesystem::path connection_ui_path;
    std::filesystem::path login_background_path;
    std::vector<ResourceCheck> resource_checks;
};

std::filesystem::path strings_path_for_lang(const std::filesystem::path& root, int lang);
std::filesystem::path login_background_for_lang(const std::filesystem::path& root, int lang, int connect_type);
ClientRuntime load_client_runtime(const std::filesystem::path& root, int lang, int connect_type);

} // namespace sphere::client
