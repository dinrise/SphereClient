#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace sphere::config {

class ConfigFile {
public:
    static ConfigFile load(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file) {
            throw std::runtime_error("failed to open config: " + path.string());
        }

        ConfigFile cfg;
        std::string line;
        while (std::getline(file, line)) {
            cfg.parse_line(line);
        }
        return cfg;
    }

    std::optional<std::string> get_string(std::string key) const {
        normalize_key(key);
        const auto it = values_.find(key);
        if (it == values_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::string get_string(std::string key, std::string fallback) const {
        auto value = get_string(std::move(key));
        return value ? *value : std::move(fallback);
    }

    int get_int(std::string key, int fallback) const {
        auto value = get_string(std::move(key));
        if (!value) {
            return fallback;
        }
        try {
            return std::stoi(*value);
        } catch (...) {
            return fallback;
        }
    }

private:
    static void normalize_key(std::string& key) {
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
    }

    static std::string trim(std::string value) {
        auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
            return !is_space(static_cast<unsigned char>(c));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
            return !is_space(static_cast<unsigned char>(c));
        }).base(), value.end());
        return value;
    }

    static std::string strip_comment(std::string line) {
        bool in_quote = false;
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            if (line[i] == '"') {
                in_quote = !in_quote;
            }
            if (!in_quote && line[i] == '/' && line[i + 1] == '/') {
                line.resize(i);
                break;
            }
        }
        return line;
    }

    static std::string unquote(std::string value) {
        value = trim(std::move(value));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            return value.substr(1, value.size() - 2);
        }
        return value;
    }

    void parse_line(std::string line) {
        line = trim(strip_comment(std::move(line)));
        if (line.empty()) {
            return;
        }

        std::istringstream input(line);
        std::string key;
        input >> key;
        if (key.empty()) {
            return;
        }

        std::string value;
        std::getline(input, value);
        value = unquote(value);

        normalize_key(key);
        values_[std::move(key)] = std::move(value);
    }

    std::unordered_map<std::string, std::string> values_;
};

} // namespace sphere::config
