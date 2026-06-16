#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sphere::client {

struct CharacterSlot {
    bool present = false;
    bool can_create = true;
    int slot = 0;
    std::wstring name;
    bool female = false;
    int face = 0;
    int hair = 0;
    int hair_color = 0;
    int tattoo = 0;
    int max_hp = 0;
    int max_mp = 0;
    int current_hp = 0;
    int current_mp = 0;
    int strength = 10;
    int dexterity = 10;
    int accuracy = 10;
    int endurance = 10;
    int fire = 0;
    int water = 0;
    int earth = 0;
    int air = 0;
    int physical_attack = 0;
    int magical_attack = 0;
    int physical_defense = 0;
    int magical_defense = 0;
};

struct CharacterCreationAppearance {
    bool female = false;
    int model_base = 0x30;
    int face = 0;
    int hair = 0;
    int hair_color = 0;
    int tattoo = 0;
};

struct CharacterAppearanceRules {
    int model_base = 0;
    int male_face_count = 0;
    int female_face_count = 0;
    int male_hair_count = 0;
    int female_hair_count = 0;
    int hair_color_count = 0;
    int tattoo_count = 0;
};

struct CharacterActionResult {
    bool ok = false;
    bool disconnected = false;
    int packet_count = 0;
    int byte_count = 0;
    std::vector<std::vector<std::uint8_t>> frames;
    std::string message;
};

struct LoginProbeResult;

class ServerSession {
public:
    ~ServerSession();

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    CharacterActionResult select_character(int slot, int timeout_ms = 2500);
    CharacterActionResult create_character(int slot, const std::wstring& name, const CharacterCreationAppearance& appearance, int timeout_ms = 2500);
    CharacterActionResult delete_character(int slot, int timeout_ms = 2500);
    CharacterActionResult send_ingame_ack(int timeout_ms = 2500);
    CharacterActionResult poll_frames(int max_frames = 32);
    bool send_position(double x, double y, double z, double angle, std::string& error);

    bool connected() const;
    std::uint16_t local_id() const;
    bool has_game_time() const;
    float game_time_fraction() const;
    void game_date(int& day, int& month, int& year) const;

private:
    struct Impl;
    explicit ServerSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend LoginProbeResult probe_login_server(
        const std::string& host,
        int port,
        const std::wstring& login,
        const std::wstring& password,
        const CharacterAppearanceRules& appearance_rules,
        int timeout_ms,
        bool debug_auto_enter);
};

struct LoginProbeResult {
    bool connected = false;
    bool legacy_handshake = false;
    int first_opcode = 0;
    int next_opcode = 0;
    int first_length = 0;
    int next_length = 0;
    bool character_select_ready = false;
    std::uint16_t local_id = 0;
    bool has_game_time = false;
    float game_time_fraction = 0.0f;
    int game_day = 0;
    int game_month = 0;
    int game_year = 0;
    int character_select_packets = 0;
    int character_select_bytes = 0;
    std::shared_ptr<ServerSession> session;
    std::array<CharacterSlot, 3> character_slots{};
    std::string message;
};

LoginProbeResult probe_login_server(
    const std::string& host,
    int port,
    const std::wstring& login,
    const std::wstring& password,
    const CharacterAppearanceRules& appearance_rules,
    int timeout_ms,
    bool debug_auto_enter);

} // namespace sphere::client
