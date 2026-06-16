#include "client/network_client.hpp"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace sphere::client {
namespace {

class WsaSession {
public:
    WsaSession() {
        ok_ = WSAStartup(MAKEWORD(2, 2), &data_) == 0;
    }

    WsaSession(const WsaSession&) = delete;
    WsaSession& operator=(const WsaSession&) = delete;

    ~WsaSession() {
        if (ok_) {
            WSACleanup();
        }
    }

    bool ok() const { return ok_; }

private:
    WSADATA data_{};
    bool ok_ = false;
};

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET socket) : socket_(socket) {}
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept {
        socket_ = other.socket_;
        other.socket_ = INVALID_SOCKET;
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset();
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET;
        }
        return *this;
    }

    ~SocketHandle() {
        reset();
    }

    explicit operator bool() const { return socket_ != INVALID_SOCKET; }
    SOCKET get() const { return socket_; }

    void reset() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

std::uint16_t read_u16le(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 1 >= data.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

void write_u16le(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xff);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

std::string wsa_error_text(const char* prefix) {
    std::ostringstream out;
    out << prefix << " (WSA " << WSAGetLastError() << ")";
    return out.str();
}

bool wait_socket(SOCKET socket, bool write, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socket, &fds);

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int rc = select(0, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &tv);
    return rc > 0 && FD_ISSET(socket, &fds);
}

SocketHandle connect_socket(const std::string& host, int port, int timeout_ms, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (gai != 0) {
        error = "resolve failed: " + std::string(gai_strerrorA(gai));
        return {};
    }

    SocketHandle connected;
    for (addrinfo* addr = addresses; addr != nullptr; addr = addr->ai_next) {
        SocketHandle socket_handle(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
        if (!socket_handle) {
            continue;
        }

        u_long nonblocking = 1;
        ioctlsocket(socket_handle.get(), FIONBIO, &nonblocking);

        const int rc = connect(socket_handle.get(), addr->ai_addr, static_cast<int>(addr->ai_addrlen));
        if (rc == 0 || WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS) {
            if (rc == 0 || wait_socket(socket_handle.get(), true, timeout_ms)) {
                int socket_error = 0;
                int socket_error_size = sizeof(socket_error);
                getsockopt(socket_handle.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &socket_error_size);
                if (socket_error == 0) {
                    nonblocking = 0;
                    ioctlsocket(socket_handle.get(), FIONBIO, &nonblocking);
                    connected = std::move(socket_handle);
                    break;
                }
                WSASetLastError(socket_error);
            }
        }
    }

    freeaddrinfo(addresses);
    if (!connected) {
        error = wsa_error_text("connect failed");
    }
    return connected;
}

bool recv_exact(SOCKET socket, std::uint8_t* out, int size, int timeout_ms) {
    int total = 0;
    while (total < size) {
        if (!wait_socket(socket, false, timeout_ms)) {
            return false;
        }
        const int rc = recv(socket, reinterpret_cast<char*>(out + total), size - total, 0);
        if (rc <= 0) {
            return false;
        }
        total += rc;
    }
    return true;
}

bool recv_frame(SOCKET socket, std::vector<std::uint8_t>& frame, int timeout_ms) {
    std::uint8_t header[2]{};
    if (!recv_exact(socket, header, 2, timeout_ms)) {
        return false;
    }

    const int length = header[0] | (header[1] << 8);
    if (length < 4 || length > 60000) {
        return false;
    }

    frame.assign(static_cast<std::size_t>(length), 0);
    frame[0] = header[0];
    frame[1] = header[1];
    return recv_exact(socket, frame.data() + 2, length - 2, timeout_ms);
}

bool send_all(SOCKET socket, const std::vector<std::uint8_t>& data) {
    int sent = 0;
    while (sent < static_cast<int>(data.size())) {
        const int rc = send(socket, reinterpret_cast<const char*>(data.data() + sent), static_cast<int>(data.size()) - sent, 0);
        if (rc <= 0) {
            return false;
        }
        sent += rc;
    }
    return true;
}

std::vector<std::uint8_t> build_legacy_packet(std::uint16_t opcode, const std::vector<std::uint8_t>& payload, std::uint16_t& sequence, std::uint16_t xor_key) {
    const std::uint16_t length = static_cast<std::uint16_t>(payload.size() + 8);
    std::vector<std::uint8_t> packet(length, 0);
    sequence = static_cast<std::uint16_t>(sequence + static_cast<std::uint16_t>((std::rand() & 3) + 1));

    write_u16le(packet, 0, length);
    write_u16le(packet, 4, sequence);
    write_u16le(packet, 6, opcode);
    std::copy(payload.begin(), payload.end(), packet.begin() + 8);

    std::int16_t sum = 0;
    for (std::size_t i = 4; i < packet.size(); ++i) {
        sum = static_cast<std::int16_t>(sum + static_cast<std::int8_t>(packet[i]));
    }
    write_u16le(packet, 2, static_cast<std::uint16_t>(xor_key ^ static_cast<std::uint16_t>(sum)));
    return packet;
}

std::vector<std::uint8_t> to_cp1251(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(1251, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, "?", nullptr);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(required > 0 ? required : 0));
    if (required > 0) {
        WideCharToMultiByte(1251, 0, text.c_str(), static_cast<int>(text.size()), reinterpret_cast<char*>(out.data()), required, "?", nullptr);
    }
    return out;
}

std::wstring from_cp1251(const std::vector<std::uint8_t>& bytes) {
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
    std::wstring out(static_cast<std::size_t>(required > 0 ? required : 0), L'\0');
    if (required > 0) {
        MultiByteToWideChar(
            1251,
            0,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            out.data(),
            required);
    }
    return out;
}

void write_byte_lsb(std::vector<std::uint8_t>& data, int bit_position, std::uint8_t value) {
    for (int bit = 0; bit < 8; ++bit) {
        if ((value & (1U << bit)) == 0) {
            continue;
        }
        const int absolute_bit = bit_position + bit;
        data[static_cast<std::size_t>(absolute_bit / 8)] |= static_cast<std::uint8_t>(1U << (absolute_bit % 8));
    }
}

std::vector<std::uint8_t> build_sphereemu_login_packet(std::uint16_t local_id, const std::wstring& login, const std::wstring& password) {
    std::vector<std::uint8_t> strings = to_cp1251(login);
    strings.push_back(1);
    const auto password_bytes = to_cp1251(password);
    strings.insert(strings.end(), password_bytes.begin(), password_bytes.end());
    strings.push_back(0);

    constexpr int string_bit_offset = 18 * 8 + 2;
    const int packet_length = (string_bit_offset + static_cast<int>(strings.size()) * 8 + 7) / 8;
    std::vector<std::uint8_t> packet(static_cast<std::size_t>(packet_length), 0);
    write_u16le(packet, 0, static_cast<std::uint16_t>(packet_length));
    write_u16le(packet, 2, 300);

    const auto major = static_cast<std::uint8_t>((local_id >> 8) & 0xff);
    const auto minor = static_cast<std::uint8_t>(local_id & 0xff);
    packet[7] = major;
    packet[8] = minor;
    packet[11] = major;
    packet[12] = minor;

    int bit_position = string_bit_offset;
    for (std::uint8_t byte : strings) {
        write_byte_lsb(packet, bit_position, byte);
        bit_position += 8;
    }
    return packet;
}

std::string make_character_name(const std::wstring& login) {
    std::string name;
    for (wchar_t ch : login) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            name.push_back(static_cast<char>(ch));
        }
        if (name.size() >= 12) {
            break;
        }
    }
    if (name.empty()) {
        name = "CodexHero";
    }
    if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) {
        name.insert(name.begin(), 'C');
    }
    if (name.size() < 3) {
        name += "Hero";
    }
    if (name.size() > 12) {
        name.resize(12);
    }
    return name;
}

std::vector<std::uint8_t> encode_character_name_check(const std::string& name) {
    std::vector<std::uint8_t> bytes(name.size() + 1, 0);
    for (std::size_t i = 1; i <= name.size(); ++i) {
        const auto code = static_cast<unsigned>(static_cast<std::uint8_t>(name[i - 1])) * 2U;
        bytes[i - 1] = static_cast<std::uint8_t>((bytes[i - 1] & 0x1fU) | ((code & 0x07U) << 5));
        bytes[i] = static_cast<std::uint8_t>((bytes[i] & 0xe0U) | ((code >> 3) & 0x1fU));
    }
    return bytes;
}

int appearance_model_value(int local_index, int model_base) {
    model_base = std::clamp(model_base, 0, 255);
    local_index = std::clamp(local_index, 0, 255 - model_base);
    return model_base + local_index;
}

int appearance_local_index(int stored_value, int count, int model_base) {
    if (count <= 0) {
        return -1;
    }
    if (stored_value >= model_base && stored_value < model_base + count) {
        return stored_value - model_base;
    }
    return -1;
}

std::array<std::uint8_t, 5> build_character_creation_bytes(const CharacterCreationAppearance& appearance) {
    const int model_base = std::clamp(appearance.model_base, 0, 255);
    const int model_face = appearance_model_value(appearance.face, model_base);
    const int model_hair = appearance_model_value(appearance.hair, model_base);
    const int model_hair_color = appearance_model_value(appearance.hair_color, model_base);
    const int model_tattoo = appearance_model_value(appearance.tattoo, model_base);

    int wire_face = model_face;
    int wire_hair = model_hair;
    int wire_hair_color = model_hair_color;
    int wire_tattoo = model_tattoo;
    if (appearance.female) {
        wire_face = 256 - model_face;
        wire_hair = 255 - model_hair;
        wire_hair_color = 255 - model_hair_color;
        wire_tattoo = 255 - model_tattoo;
    }

    std::array<std::uint8_t, 5> bytes{};
    bytes[0] = static_cast<std::uint8_t>((wire_face & 0x03) << 6);
    bytes[1] = static_cast<std::uint8_t>(((wire_face >> 2) & 0x3f) | ((wire_hair & 0x03) << 6));
    bytes[2] = static_cast<std::uint8_t>(((wire_hair >> 2) & 0x3f) | ((wire_hair_color & 0x03) << 6));
    bytes[3] = static_cast<std::uint8_t>(((wire_hair_color >> 2) & 0x3f) | ((wire_tattoo & 0x03) << 6));
    bytes[4] = static_cast<std::uint8_t>((wire_tattoo >> 2) & 0x3f);
    return bytes;
}

std::vector<std::uint8_t> build_character_select_packet(std::uint16_t local_id, int slot) {
    constexpr std::uint16_t length = 0x15;
    std::vector<std::uint8_t> packet(length, 0);
    write_u16le(packet, 0, length);
    write_u16le(packet, 2, 300);
    packet[6] = 0x04;
    packet[7] = static_cast<std::uint8_t>((local_id >> 8) & 0xff);
    packet[8] = static_cast<std::uint8_t>(local_id & 0xff);
    packet[9] = 0x08;
    packet[10] = 0x40;
    packet[17] = static_cast<std::uint8_t>((std::clamp(slot, 0, 2) + 1) * 4);
    return packet;
}

std::vector<std::uint8_t> build_create_character_packet(std::uint16_t local_id, int slot, const std::string& name, const CharacterCreationAppearance& appearance) {
    const auto name_check = encode_character_name_check(name);
    const auto length = static_cast<std::uint16_t>(25 + name_check.size());
    std::vector<std::uint8_t> packet(length, 0);
    write_u16le(packet, 0, length);
    write_u16le(packet, 2, 300);
    packet[6] = 0x04;
    packet[7] = static_cast<std::uint8_t>((local_id >> 8) & 0xff);
    packet[8] = static_cast<std::uint8_t>(local_id & 0xff);
    packet[9] = 0x08;
    packet[10] = 0x40;
    packet[13] = 0x08;
    packet[14] = 0x40;
    packet[15] = 0x80;
    packet[16] = 0x05;
    packet[17] = static_cast<std::uint8_t>((std::clamp(slot, 0, 2) + 1) * 4);
    std::copy(name_check.begin(), name_check.end(), packet.begin() + 20);
    const auto char_data = build_character_creation_bytes(appearance);
    std::copy(char_data.begin(), char_data.end(), packet.end() - char_data.size());
    return packet;
}

std::vector<std::uint8_t> build_delete_character_packet(std::uint16_t local_id, int slot) {
    constexpr std::uint16_t length = 0x2a;
    std::vector<std::uint8_t> packet(length, 0);
    write_u16le(packet, 0, length);
    write_u16le(packet, 2, 300);
    packet[6] = 0x04;
    packet[7] = static_cast<std::uint8_t>((local_id >> 8) & 0xff);
    packet[8] = static_cast<std::uint8_t>(local_id & 0xff);
    packet[9] = 0x08;
    packet[10] = 0x40;
    packet[13] = 0x08;
    packet[14] = 0x40;
    packet[15] = 0x80;
    packet[16] = 0x0d;
    packet[17] = static_cast<std::uint8_t>((std::clamp(slot, 0, 2) + 1) * 4);
    return packet;
}

std::vector<std::uint8_t> build_ingame_ack_packet(std::uint16_t local_id) {
    constexpr std::uint16_t length = 0x13;
    std::vector<std::uint8_t> packet(length, 0);
    write_u16le(packet, 0, length);
    write_u16le(packet, 2, 300);
    packet[6] = 0x04;
    packet[7] = static_cast<std::uint8_t>((local_id >> 8) & 0xff);
    packet[8] = static_cast<std::uint8_t>(local_id & 0xff);
    packet[9] = 0x08;
    packet[10] = 0x40;
    return packet;
}

void write_client_coordinate(std::vector<std::uint8_t>& packet, std::size_t offset, double value) {
    constexpr double fraction_base = 8388608.0;
    std::uint32_t fraction = 0;
    int scale = 126;
    bool negative = false;
    if (std::abs(value) > 0.0000001) {
        negative = value < 0.0;
        const double absolute = std::abs(value);
        const int exponent = static_cast<int>(std::floor(std::log2(absolute)));
        scale = std::clamp(exponent + 127, 0, 255);
        const double normalized = absolute / std::ldexp(1.0, exponent);
        fraction = static_cast<std::uint32_t>((normalized - 1.0) * fraction_base) & 0x7fffffU;
    }

    packet[offset] = static_cast<std::uint8_t>((packet[offset] & 0x3fU) | ((fraction & 0x3U) << 6));
    packet[offset + 1] = static_cast<std::uint8_t>((fraction >> 2) & 0xffU);
    packet[offset + 2] = static_cast<std::uint8_t>((fraction >> 10) & 0xffU);
    packet[offset + 3] = static_cast<std::uint8_t>(((fraction >> 18) & 0x1fU) | ((scale & 0x7) << 5));
    packet[offset + 4] = static_cast<std::uint8_t>(
        (packet[offset + 4] & 0xc0U) | ((scale >> 3) & 0x1fU) | (negative ? 0x20U : 0U));
}

std::vector<std::uint8_t> build_position_packet(
    std::uint16_t local_id,
    std::uint8_t sequence,
    double x,
    double y,
    double z,
    double angle) {
    constexpr std::uint16_t length = 0x26;
    std::vector<std::uint8_t> packet(length, 0);
    write_u16le(packet, 0, length);
    write_u16le(packet, 2, 300);
    packet[6] = 0x04;
    packet[9] = 0x08;
    packet[10] = 0x40;
    // In-game position packets carry the local id here and are not XOR encoded.
    packet[11] = static_cast<std::uint8_t>((local_id >> 8) & 0xff);
    packet[12] = static_cast<std::uint8_t>(local_id & 0xff);
    packet[17] = sequence;
    write_client_coordinate(packet, 21, x);
    write_client_coordinate(packet, 25, y);
    write_client_coordinate(packet, 29, z);
    write_client_coordinate(packet, 33, angle);
    return packet;
}

std::vector<std::uint8_t> encode_client_packet_for_sphereemu(const std::vector<std::uint8_t>& decoded) {
    constexpr std::uint8_t encoding_mask[] = {0x4B, 0x0D, 0xEF, 0x60, 0xC9, 0x9A, 0x70, 0x0E, 0x03};
    constexpr std::size_t start = 9;
    if (decoded.size() <= start) {
        return decoded;
    }

    std::vector<std::uint8_t> encoded = decoded;
    std::uint8_t mask3 = 0;
    for (std::size_t i = 0; i + start < decoded.size(); ++i) {
        const std::uint8_t current = decoded[i + start];
        encoded[i + start] = static_cast<std::uint8_t>(current ^ encoding_mask[i % std::size(encoding_mask)] ^ mask3);
        mask3 = static_cast<std::uint8_t>(current * i + 2 * mask3);
    }
    return encoded;
}

bool looks_like_cannot_connect(const std::vector<std::uint8_t>& frame) {
    return frame.size() == 14 &&
           read_u16le(frame, 2) == 300 &&
           frame[9] == 0x08 &&
           frame[10] == 0x40 &&
           frame[11] == 0xA0;
}

bool looks_like_character_select_start(const std::vector<std::uint8_t>& frame) {
    return frame.size() == 82 &&
           read_u16le(frame, 2) == 300 &&
           frame[9] == 0x08 &&
           frame[10] == 0x40 &&
           frame[11] == 0x80 &&
           frame[12] == 0x10;
}

bool looks_like_empty_character_slot(const std::vector<std::uint8_t>& frame) {
    return frame.size() == 108 &&
           read_u16le(frame, 2) == 300 &&
           frame[9] == 0x08 &&
           frame[10] == 0x40 &&
           frame[11] == 0x60 &&
           frame[12] == 0x79;
}

bool looks_like_character_slot(const std::vector<std::uint8_t>& frame) {
    return frame.size() == 108 &&
           read_u16le(frame, 2) == 300 &&
           frame[9] == 0x08 &&
           frame[10] == 0x40 &&
           frame[11] == 0x60;
}

int read_packed14(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 1 >= data.size()) {
        return 0;
    }
    return static_cast<int>((data[offset] >> 2) | (data[offset + 1] << 6));
}

std::wstring decode_slot_name(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 87) {
        return {};
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(19);
    for (std::size_t i = 0; i < 19; ++i) {
        const std::uint8_t current = frame[67 + i];
        const std::uint8_t next = i == 18 ? frame[86] : frame[68 + i];
        const auto decoded = static_cast<std::uint8_t>((current >> 2) | ((next & 0x03) << 6));
        if (decoded == 0) {
            break;
        }
        bytes.push_back(decoded);
    }
    return from_cp1251(bytes);
}

CharacterSlot parse_character_slot(const std::vector<std::uint8_t>& frame, int slot, const CharacterAppearanceRules& appearance_rules) {
    CharacterSlot out;
    out.slot = slot;
    out.can_create = frame.size() == 108 && frame[12] == 0x79;
    if (!looks_like_character_slot(frame)) {
        out.can_create = false;
        return out;
    }

    out.name = decode_slot_name(frame);
    out.present = !out.name.empty() && frame[12] == 0x79;
    out.female = frame.size() > 66 && (((frame[66] >> 2) & 1) != 0);
    out.max_hp = read_packed14(frame, 13);
    out.max_mp = read_packed14(frame, 15);
    out.strength = read_packed14(frame, 17);
    out.dexterity = read_packed14(frame, 19);
    out.accuracy = read_packed14(frame, 21);
    out.endurance = read_packed14(frame, 23);
    out.earth = read_packed14(frame, 25);
    out.air = read_packed14(frame, 27);
    out.water = read_packed14(frame, 29);
    out.fire = read_packed14(frame, 31);
    out.physical_defense = read_packed14(frame, 33);
    out.magical_defense = read_packed14(frame, 35);
    out.current_hp = read_packed14(frame, 54);
    out.current_mp = read_packed14(frame, 56);

    if (frame.size() > 90) {
        out.face = static_cast<int>((frame[86] >> 2) | ((frame[87] & 0x03) << 6));
        out.hair = static_cast<int>((frame[87] >> 2) | ((frame[88] & 0x03) << 6));
        out.hair_color = static_cast<int>((frame[88] >> 2) | ((frame[89] & 0x03) << 6));
        out.tattoo = static_cast<int>((frame[89] >> 2) | ((frame[90] & 0x03) << 6));
        out.face = appearance_local_index(
            out.face,
            out.female ? appearance_rules.female_face_count : appearance_rules.male_face_count,
            appearance_rules.model_base);
        out.hair = appearance_local_index(
            out.hair,
            out.female ? appearance_rules.female_hair_count : appearance_rules.male_hair_count,
            appearance_rules.model_base);
        out.hair_color = appearance_local_index(out.hair_color, appearance_rules.hair_color_count, appearance_rules.model_base);
        out.tattoo = appearance_local_index(out.tattoo, appearance_rules.tattoo_count, appearance_rules.model_base);
    }

    return out;
}

std::string cp1251_string(const std::wstring& text) {
    const auto bytes = to_cp1251(text);
    return std::string(bytes.begin(), bytes.end());
}

void read_action_frames(SOCKET socket, CharacterActionResult& result, int max_frames, int timeout_ms) {
    std::vector<std::uint8_t> frame;
    while (result.packet_count < max_frames && recv_frame(socket, frame, timeout_ms)) {
        ++result.packet_count;
        result.byte_count += static_cast<int>(frame.size());
        result.frames.push_back(frame);
    }
}

void read_available_frames(SOCKET socket, CharacterActionResult& result, int max_frames) {
    while (result.packet_count < max_frames) {
        u_long available = 0;
        if (ioctlsocket(socket, FIONREAD, &available) != 0 || available < 2) {
            break;
        }
        std::uint8_t header[2]{};
        if (recv(socket, reinterpret_cast<char*>(header), 2, MSG_PEEK) != 2) {
            break;
        }
        const int length = header[0] | (header[1] << 8);
        if (length < 4 || length > 60000 || available < static_cast<u_long>(length)) {
            break;
        }
        std::vector<std::uint8_t> frame;
        if (!recv_frame(socket, frame, 0)) {
            break;
        }
        ++result.packet_count;
        result.byte_count += static_cast<int>(frame.size());
        result.frames.push_back(std::move(frame));
    }
}

// Decode the in-game date/time from the server credentials (opcode 300) packet.
// Layout is the inverse of SphServer TimeHelper.EncodeCurrentSphereDateTime:
//   frame[13] = (minute&0x0f)<<4 | (second/12+1)
//   frame[14] = (day&1)<<7 | hour<<2 | (minute>>4)&0x03
//   frame[15] = month<<4 | (day>>1)&0x0f
//   frame[16] = year & 0xff
//   frame[17] = 0x34 | ((year>>8)&0x03)
// The raw year is the .NET DateTime year (~334); the client displays it +7800
// (matches the original client's "...8134" and the project's PacketLogViewer).
bool decode_server_credentials_time(const std::vector<std::uint8_t>& frame, float& fraction,
                                    int& day, int& month, int& year) {
    if (frame.size() != 56 || read_u16le(frame, 2) != 300 ||
        frame[9] != 0x08 || frame[10] != 0x40 || frame[11] != 0x20 || frame[12] != 0x10) {
        return false;
    }
    const int seconds = ((frame[13] & 0x0f) - 1) * 12;
    const int minutes = ((frame[14] & 0x03) << 4) | (frame[13] >> 4);
    const int hours = (frame[14] >> 2) & 0x1f;
    if (seconds < 0 || seconds >= 60 || minutes >= 60 || hours >= 24) {
        return false;
    }
    fraction = static_cast<float>(hours * 3600 + minutes * 60 + seconds) / 86400.0f;
    day = ((frame[15] & 0x0f) << 1) | ((frame[14] >> 7) & 0x01);
    month = (frame[15] >> 4) & 0x0f;
    year = ((((frame[17] & 0x03) << 8) | frame[16]) + 7800);
    return true;
}

bool is_socket_connected(const SocketHandle& socket) {
    return static_cast<bool>(socket);
}

} // namespace

struct ServerSession::Impl {
    WsaSession wsa;
    SocketHandle socket;
    std::uint16_t local_id = 0;
    std::uint8_t position_sequence = 0;
    bool has_game_time = false;
    float game_time_fraction = 0.0f;
    int game_day = 0;
    int game_month = 0;
    int game_year = 0;
};

ServerSession::ServerSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

ServerSession::~ServerSession() = default;

bool ServerSession::connected() const {
    return impl_ && is_socket_connected(impl_->socket);
}

std::uint16_t ServerSession::local_id() const {
    return impl_ ? impl_->local_id : 0;
}

bool ServerSession::has_game_time() const {
    return impl_ && impl_->has_game_time;
}

float ServerSession::game_time_fraction() const {
    return impl_ ? impl_->game_time_fraction : 0.0f;
}

void ServerSession::game_date(int& day, int& month, int& year) const {
    day = impl_ ? impl_->game_day : 0;
    month = impl_ ? impl_->game_month : 0;
    year = impl_ ? impl_->game_year : 0;
}

CharacterActionResult ServerSession::select_character(int slot, int timeout_ms) {
    CharacterActionResult result;
    if (!connected()) {
        result.message = "character session is closed";
        return result;
    }

    const auto packet = encode_client_packet_for_sphereemu(build_character_select_packet(impl_->local_id, slot));
    if (!send_all(impl_->socket.get(), packet)) {
        result.message = wsa_error_text("character select send failed");
        return result;
    }

    read_action_frames(impl_->socket.get(), result, 1, timeout_ms);
    result.ok = result.packet_count > 0;
    std::ostringstream out;
    out << "selected slot " << (std::clamp(slot, 0, 2) + 1)
        << "; character packets=" << result.packet_count
        << " bytes=" << result.byte_count;
    result.message = out.str();
    return result;
}

CharacterActionResult ServerSession::create_character(int slot, const std::wstring& name, const CharacterCreationAppearance& appearance, int timeout_ms) {
    CharacterActionResult result;
    if (!connected()) {
        result.message = "character session is closed";
        return result;
    }

    const auto name_bytes = cp1251_string(name);
    const auto packet = encode_client_packet_for_sphereemu(build_create_character_packet(impl_->local_id, slot, name_bytes, appearance));
    if (!send_all(impl_->socket.get(), packet)) {
        result.message = wsa_error_text("character create send failed");
        return result;
    }

    read_action_frames(impl_->socket.get(), result, 2, timeout_ms);
    result.ok = result.packet_count >= 2;
    std::ostringstream out;
    out << "created slot " << (std::clamp(slot, 0, 2) + 1)
        << "; character packets=" << result.packet_count
        << " bytes=" << result.byte_count;
    result.message = out.str();
    return result;
}

CharacterActionResult ServerSession::delete_character(int slot, int timeout_ms) {
    CharacterActionResult result;
    if (!connected()) {
        result.message = "character session is closed";
        return result;
    }

    const auto packet = encode_client_packet_for_sphereemu(build_delete_character_packet(impl_->local_id, slot));
    if (!send_all(impl_->socket.get(), packet)) {
        result.message = wsa_error_text("character delete send failed");
        return result;
    }

    read_action_frames(impl_->socket.get(), result, 1, timeout_ms);
    result.ok = true;
    result.disconnected = result.packet_count == 0;
    std::ostringstream out;
    out << "delete sent for slot " << (std::clamp(slot, 0, 2) + 1);
    if (result.disconnected) {
        out << "; server closed/restarted character session";
        impl_->socket.reset();
    } else {
        out << "; packets=" << result.packet_count << " bytes=" << result.byte_count;
    }
    result.message = out.str();
    return result;
}

CharacterActionResult ServerSession::send_ingame_ack(int timeout_ms) {
    CharacterActionResult result;
    if (!connected()) {
        result.message = "character session is closed";
        return result;
    }

    const auto packet = encode_client_packet_for_sphereemu(build_ingame_ack_packet(impl_->local_id));
    if (!send_all(impl_->socket.get(), packet)) {
        result.message = wsa_error_text("ingame ACK send failed");
        return result;
    }

    read_action_frames(impl_->socket.get(), result, 8, timeout_ms);
    result.ok = true;
    std::ostringstream out;
    out << "ingame ACK sent; world packets=" << result.packet_count
        << " bytes=" << result.byte_count;
    result.message = out.str();
    return result;
}

CharacterActionResult ServerSession::poll_frames(int max_frames) {
    CharacterActionResult result;
    if (!connected()) {
        result.message = "character session is closed";
        return result;
    }
    read_available_frames(impl_->socket.get(), result, (std::max)(1, max_frames));
    result.ok = true;
    return result;
}

bool ServerSession::send_position(double x, double y, double z, double angle, std::string& error) {
    if (!connected()) {
        error = "character session is closed";
        return false;
    }
    const auto packet = build_position_packet(impl_->local_id, ++impl_->position_sequence, x, y, z, angle);
    if (!send_all(impl_->socket.get(), packet)) {
        error = wsa_error_text("position send failed");
        return false;
    }
    return true;
}

LoginProbeResult probe_login_server(
    const std::string& host,
    int port,
    const std::wstring& login,
    const std::wstring& password,
    const CharacterAppearanceRules& appearance_rules,
    int timeout_ms,
    bool debug_auto_enter) {
    LoginProbeResult result;
    auto session_impl = std::make_unique<ServerSession::Impl>();
    if (!session_impl->wsa.ok()) {
        result.message = wsa_error_text("WSAStartup failed");
        return result;
    }

    std::string error;
    session_impl->socket = connect_socket(host, port, timeout_ms, error);
    if (!session_impl->socket) {
        result.message = error;
        return result;
    }
    result.connected = true;
    SOCKET socket = session_impl->socket.get();

    std::vector<std::uint8_t> first_frame;
    if (!recv_frame(socket, first_frame, timeout_ms)) {
        result.message = "TCP connected; no initial server frame";
        return result;
    }

    result.first_length = static_cast<int>(first_frame.size());
    result.first_opcode = read_u16le(first_frame, 2);
    if (result.first_opcode == 100) {
        result.message = "Server refused connection: connection limit";
        return result;
    }
    if (result.first_opcode != 200) {
        std::ostringstream out;
        out << "TCP connected; unexpected first opcode " << result.first_opcode
            << " len=" << result.first_length;
        result.message = out.str();
        return result;
    }

    result.legacy_handshake = true;
    const std::uint16_t xor_key = read_u16le(first_frame, 8);
    std::uint16_t sequence = static_cast<std::uint16_t>((std::rand() % 1000) + 1);
    const std::vector<std::uint8_t> connection_ack{3, 0, 0, 0};
    const auto ack_packet = build_legacy_packet(400, connection_ack, sequence, xor_key);
    if (!send_all(socket, ack_packet)) {
        result.message = wsa_error_text("legacy ACK send failed");
        return result;
    }

    std::vector<std::uint8_t> next_frame;
    if (recv_frame(socket, next_frame, timeout_ms)) {
        result.next_length = static_cast<int>(next_frame.size());
        result.next_opcode = read_u16le(next_frame, 2);
        result.has_game_time = decode_server_credentials_time(
            next_frame, result.game_time_fraction, result.game_day, result.game_month, result.game_year);
        session_impl->has_game_time = result.has_game_time;
        session_impl->game_time_fraction = result.game_time_fraction;
        session_impl->game_day = result.game_day;
        session_impl->game_month = result.game_month;
        session_impl->game_year = result.game_year;
    }

    if (result.next_opcode == 300 && next_frame.size() >= 13 && !login.empty() && !password.empty()) {
        Sleep(250);
        const auto local_id = static_cast<std::uint16_t>((next_frame[7] << 8) | next_frame[8]);
        const auto decoded_login_packet = build_sphereemu_login_packet(local_id, login, password);
        const auto login_packet = encode_client_packet_for_sphereemu(decoded_login_packet);
        if (!send_all(socket, login_packet)) {
            result.message = wsa_error_text("login packet send failed");
            return result;
        }

        std::vector<std::uint8_t> login_response;
        if (recv_frame(socket, login_response, timeout_ms * 3)) {
            result.next_length = static_cast<int>(login_response.size());
            result.next_opcode = read_u16le(login_response, 2);
            if (looks_like_cannot_connect(login_response)) {
                std::ostringstream out;
                out << "Login rejected by server; localId=0x" << std::hex << local_id << std::dec;
                result.message = out.str();
                return result;
            }

            if (looks_like_character_select_start(login_response)) {
                int extra_frames = 0;
                int extra_bytes = 0;
                int slot_frames = 0;
                std::vector<std::uint8_t> extra_frame;
                std::vector<std::uint8_t> first_character_packet;
                while (extra_frames < 8 && slot_frames < 3 && recv_frame(socket, extra_frame, 500)) {
                    if (first_character_packet.empty()) {
                        first_character_packet = extra_frame;
                    }
                    ++extra_frames;
                    extra_bytes += static_cast<int>(extra_frame.size());
                    if (looks_like_character_slot(extra_frame)) {
                        result.character_slots[static_cast<std::size_t>(slot_frames)] = parse_character_slot(extra_frame, slot_frames, appearance_rules);
                        ++slot_frames;
                    }
                }

                if (!debug_auto_enter) {
                    result.character_select_ready = slot_frames == 3;
                    result.local_id = local_id;
                    result.character_select_packets = extra_frames;
                    result.character_select_bytes = extra_bytes;
                    session_impl->local_id = local_id;
                    result.session = std::shared_ptr<ServerSession>(new ServerSession(std::move(session_impl)));
                    std::ostringstream out;
                    out << "Login OK; character select data received; localId=0x" << std::hex << local_id << std::dec
                        << "; packets=" << extra_frames
                        << " bytes=" << extra_bytes
                        << "; slots=" << slot_frames
                        << "; next step is 3D scene";
                    result.message = out.str();
                    return result;
                }

                const bool should_create_character = slot_frames > 0 && !result.character_slots[0].present && result.character_slots[0].can_create;
                const auto character_name = make_character_name(login);
                CharacterCreationAppearance appearance;
                appearance.model_base = appearance_rules.model_base;
                const auto decoded_character_packet = should_create_character
                    ? build_create_character_packet(local_id, 0, character_name, appearance)
                    : build_character_select_packet(local_id, 0);
                const auto character_packet = encode_client_packet_for_sphereemu(decoded_character_packet);
                if (!send_all(socket, character_packet)) {
                    result.message = wsa_error_text("character select send failed");
                    return result;
                }

                int character_frames = 0;
                int character_bytes = 0;
                std::vector<std::uint8_t> character_frame;
                while (character_frames < 8 && recv_frame(socket, character_frame, 1000)) {
                    ++character_frames;
                    character_bytes += static_cast<int>(character_frame.size());
                    if (character_frames >= 2) {
                        break;
                    }
                }

                const auto ingame_ack = encode_client_packet_for_sphereemu(build_ingame_ack_packet(local_id));
                int world_frames = 0;
                int world_bytes = 0;
                if (character_frames > 0 && send_all(socket, ingame_ack)) {
                    std::vector<std::uint8_t> world_frame;
                    while (world_frames < 8 && recv_frame(socket, world_frame, 1000)) {
                        ++world_frames;
                        world_bytes += static_cast<int>(world_frame.size());
                    }
                }

                std::ostringstream out;
                out << "Login OK; character select start; localId=0x" << std::hex << local_id << std::dec
                    << "; extra packets=" << extra_frames
                    << " bytes=" << extra_bytes
                    << "; " << (should_create_character ? "created " : "selected ")
                    << character_name
                    << "; character packets=" << character_frames
                    << " bytes=" << character_bytes
                    << "; world packets=" << world_frames
                    << " bytes=" << world_bytes;
                result.message = out.str();
                return result;
            }

            std::ostringstream out;
            out << "Encoded login sent; localId=0x" << std::hex << local_id << std::dec
                << "; server opcode=" << result.next_opcode
                << " len=" << result.next_length;
            result.message = out.str();
            return result;
        }

        std::ostringstream out;
        out << "Encoded login sent; localId=0x" << std::hex << local_id << std::dec
            << "; waiting for server response";
        result.message = out.str();
        return result;
    }

    std::ostringstream out;
    out << "Legacy TCP handshake OK";
    if (result.next_opcode != 0) {
        out << "; next opcode=" << result.next_opcode << " len=" << result.next_length;
    } else {
        out << "; waiting for server data";
    }
    result.message = out.str();
    return result;
}

} // namespace sphere::client
