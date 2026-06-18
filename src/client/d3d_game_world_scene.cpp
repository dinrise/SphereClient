#include "client/d3d_game_world_scene.hpp"

#include "client/skinned_character.hpp"
#include "common/binary_reader.hpp"
#include "model/mdl.hpp"

#include <d3d9.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sphere::client {
namespace {

constexpr DWORD kWorldVertexFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2;
constexpr DWORD kOverlayVertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr std::size_t kMapRecordBytes = 22;
constexpr std::size_t kLndHeaderBytes = 0x68c0;
constexpr std::size_t kLndVertexBytes = 40;
constexpr std::size_t kLndTriangleBytes = 28;
constexpr float kPi = 3.14159265358979323846f;
// SKL "free"/idle action used while the player stands still. Other action
// indices are selected by movement state (see update_player_animation).
constexpr std::size_t kPlayerIdleAction = 20;
constexpr float kPlayerAnimSecondsPerFrame = 0.08f; // matches character-select 80ms/frame
// First-person eye placement, in world units (model ~2.05 tall, +y down).
// The eye sits just below the crown of the skinned body (so it lands at eye
// level and the near clip plane culls the player's own head), nudged forward
// toward the face front.
constexpr float kEyeBelowCrownWorld = 0.16f;  // distance down from the crown
constexpr float kEyeForwardModel = 0.18f;      // toward the face front (model +z)
// While moving, the body leans forward and its neck cut would ride into the
// lens, so (like the original) the model snaps backward along the look axis.
constexpr float kMoveBodyBackShift = 0.5f;
// Walk bob: while moving, the eye follows the head-top's vertical motion through
// the walk/run cycle (as the original's head-attached camera does), giving a
// small up/down sway. Idle keeps the eye locked. No start/stop offset.
constexpr float kWalkBobScale = 1.0f;
// Jump impulse and gravity are data-driven (config.jump_impulse / jump_gravity,
// from game_window.lua). Original values, verified in Ghidra: ControlMove sets
// the vertical-velocity field 0x28C to -5 on jump (potions scale it ×2/×4 via the
// i4/i5 effect flags); the native physics FUN_004755e0 integrates vy += g*dt with
// g = the double at 0x00504248 = 9.8.

struct WorldVertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    DWORD diffuse;
    float u;
    float v;
    float detail_u;
    float detail_v;
};

struct OverlayVertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD diffuse;
    float u;
    float v;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

struct TerrainResource {
    std::filesystem::path lnd_path;
    IDirect3DVertexBuffer9* vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* index_buffer = nullptr;
    IDirect3DTexture9* texture = nullptr;
    UINT vertex_count = 0;
    UINT index_count = 0;
    std::vector<Vec3> positions;
    std::vector<std::uint16_t> indices;
    // Water surface for this patch (from the matching .wtr; 12x12 height grid,
    // value >=900 = no water). Empty when the patch has no water.
    IDirect3DVertexBuffer9* water_vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* water_index_buffer = nullptr;
    UINT water_vertex_count = 0;
    UINT water_index_count = 0;
    float water_height = 0.0f;   // representative water-surface Y for this patch (planar-reflection plane)
    bool has_water = false;
    std::vector<WorldVertex> water_cpu_verts;  // base water verts (for per-frame wave Y displacement)
};

struct TerrainInstance {
    TerrainResource* resource = nullptr;
    float origin_x = 0.0f;
    float origin_z = 0.0f;
};

struct StaticModelBatch {
    UINT start_index = 0;
    UINT index_count = 0;
    IDirect3DTexture9* texture = nullptr;
};

struct PlayerBatch {
    UINT start_index = 0;
    UINT index_count = 0;
    IDirect3DTexture9* texture = nullptr;
    bool is_head = false;
};

struct StaticModelResource {
    IDirect3DVertexBuffer9* vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* index_buffer = nullptr;
    UINT vertex_count = 0;
    std::vector<StaticModelBatch> batches;
    std::vector<Vec3> collision_positions;
    std::vector<std::uint16_t> collision_indices;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
};

struct StaticPlacement {
    std::string model_name;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

struct StaticInstance {
    StaticModelResource* resource = nullptr;
    D3DMATRIX world{};
    Vec3 bounds_min{};
    Vec3 bounds_max{};
};

struct GrassInstance {
    StaticModelResource* resource = nullptr;
    D3DMATRIX world{};
    float wind_phase = 0.0f;
    float wind_scale = 1.0f;
    int cell_x = 0;
    int cell_z = 0;
};

template <typename T>
void release_com(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 normalize(Vec3 value) {
    const float length = std::sqrt(dot(value, value));
    if (length <= 0.00001f) {
        return Vec3{0.0f, 1.0f, 0.0f};
    }
    return Vec3{value.x / length, value.y / length, value.z / length};
}

Vec3 subtract(Vec3 left, Vec3 right) {
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 add(Vec3 left, Vec3 right) {
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 scale(Vec3 value, float factor) {
    return Vec3{value.x * factor, value.y * factor, value.z * factor};
}

Vec3 transform_point(Vec3 point, const D3DMATRIX& matrix) {
    return Vec3{
        point.x * matrix._11 + point.y * matrix._21 + point.z * matrix._31 + matrix._41,
        point.x * matrix._12 + point.y * matrix._22 + point.z * matrix._32 + matrix._42,
        point.x * matrix._13 + point.y * matrix._23 + point.z * matrix._33 + matrix._43,
    };
}

float point_triangle_distance_squared(Vec3 point, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = subtract(b, a);
    const Vec3 ac = subtract(c, a);
    const Vec3 ap = subtract(point, a);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return dot(ap, ap);
    }

    const Vec3 bp = subtract(point, b);
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return dot(bp, bp);
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        const Vec3 delta = subtract(point, add(a, scale(ab, v)));
        return dot(delta, delta);
    }

    const Vec3 cp = subtract(point, c);
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return dot(cp, cp);
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        const Vec3 delta = subtract(point, add(a, scale(ac, w)));
        return dot(delta, delta);
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const Vec3 bc = subtract(c, b);
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const Vec3 delta = subtract(point, add(b, scale(bc, w)));
        return dot(delta, delta);
    }

    const float denominator = 1.0f / (va + vb + vc);
    const float v = vb * denominator;
    const float w = vc * denominator;
    const Vec3 delta = subtract(point, add(a, add(scale(ab, v), scale(ac, w))));
    return dot(delta, delta);
}

D3DMATRIX identity_matrix() {
    D3DMATRIX matrix{};
    matrix._11 = 1.0f;
    matrix._22 = 1.0f;
    matrix._33 = 1.0f;
    matrix._44 = 1.0f;
    return matrix;
}

D3DMATRIX translation_matrix(float x, float y, float z) {
    D3DMATRIX matrix = identity_matrix();
    matrix._41 = x;
    matrix._42 = y;
    matrix._43 = z;
    return matrix;
}

D3DMATRIX multiply_matrix(const D3DMATRIX& left, const D3DMATRIX& right) {
    D3DMATRIX out{};
    const auto* a = reinterpret_cast<const float*>(&left);
    const auto* b = reinterpret_cast<const float*>(&right);
    auto* result = reinterpret_cast<float*>(&out);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = 0.0f;
            for (int i = 0; i < 4; ++i) {
                value += a[row * 4 + i] * b[i * 4 + column];
            }
            result[row * 4 + column] = value;
        }
    }
    return out;
}

D3DMATRIX transpose_matrix(const D3DMATRIX& m) {
    D3DMATRIX out{};
    const auto* a = reinterpret_cast<const float*>(&m);
    auto* r = reinterpret_cast<float*>(&out);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r[row * 4 + col] = a[col * 4 + row];
        }
    }
    return out;
}

// Parse the CTAB (constant table) embedded in a compiled D3D9 shader's comment
// stream, returning a name -> float-constant-register map. Register assignment
// differs per shader permutation, so constants must be looked up by name.
std::unordered_map<std::string, int> parse_shader_constants(const std::vector<std::uint8_t>& code) {
    std::unordered_map<std::string, int> out;
    if (code.size() < 8) {
        return out;
    }
    auto rd = [&](std::size_t o) -> std::uint32_t {
        return static_cast<std::uint32_t>(code[o]) | (static_cast<std::uint32_t>(code[o + 1]) << 8) |
               (static_cast<std::uint32_t>(code[o + 2]) << 16) | (static_cast<std::uint32_t>(code[o + 3]) << 24);
    };
    std::size_t off = 4;  // skip version token
    while (off + 4 <= code.size()) {
        const std::uint32_t tok = rd(off);
        if (tok == 0x0000FFFF) {
            break;  // END
        }
        if ((tok & 0xFFFF) == 0xFFFE) {  // comment token
            const std::uint32_t clen = (tok >> 16) & 0x7FFF;
            const std::size_t cdata = off + 4;
            if (cdata + 4 <= code.size() && rd(cdata) == 0x42415443) {  // 'CTAB'
                const std::size_t ct = cdata + 4;  // CTAB blob base (offsets are relative to here)
                const std::uint32_t nconst = rd(ct + 12);
                const std::uint32_t cinfo = rd(ct + 16);
                for (std::uint32_t i = 0; i < nconst; ++i) {
                    const std::size_t e = ct + cinfo + i * 20;
                    if (e + 20 > code.size()) {
                        break;
                    }
                    const std::uint32_t name_off = rd(e);
                    const std::uint16_t reg_set = static_cast<std::uint16_t>(code[e + 4] | (code[e + 5] << 8));
                    const std::uint16_t reg_idx = static_cast<std::uint16_t>(code[e + 6] | (code[e + 7] << 8));
                    if (reg_set != 2) {  // only float (c#) registers
                        continue;
                    }
                    std::string name;
                    for (std::size_t p = ct + name_off; p < code.size() && code[p]; ++p) {
                        name.push_back(static_cast<char>(code[p]));
                    }
                    out[name] = reg_idx;
                }
            }
            off += 1 + clen;
            continue;
        }
        off += 1 + ((tok >> 24) & 0x0F);  // normal instruction length
    }
    return out;
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::vector<std::uint8_t> data;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return data;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), size);
    }
    return data;
}

D3DMATRIX rotation_x_matrix(float radians) {
    D3DMATRIX matrix = identity_matrix();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix._22 = c;
    matrix._23 = s;
    matrix._32 = -s;
    matrix._33 = c;
    return matrix;
}

D3DMATRIX rotation_y_matrix(float radians) {
    D3DMATRIX matrix = identity_matrix();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix._11 = c;
    matrix._13 = -s;
    matrix._31 = s;
    matrix._33 = c;
    return matrix;
}

D3DMATRIX rotation_z_matrix(float radians) {
    D3DMATRIX matrix = identity_matrix();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix._11 = c;
    matrix._12 = s;
    matrix._21 = -s;
    matrix._22 = c;
    return matrix;
}

D3DMATRIX scale_matrix(float scale) {
    D3DMATRIX matrix = identity_matrix();
    matrix._11 = scale;
    matrix._22 = scale;
    matrix._33 = scale;
    return matrix;
}

D3DMATRIX align_up_matrix(Vec3 normal) {
    normal = normalize(normal);
    Vec3 tangent = cross(normal, Vec3{0.0f, 0.0f, 1.0f});
    if (dot(tangent, tangent) <= 0.00001f) {
        tangent = cross(normal, Vec3{1.0f, 0.0f, 0.0f});
    }
    tangent = normalize(tangent);
    const Vec3 bitangent = normalize(cross(tangent, normal));
    auto matrix = identity_matrix();
    matrix._11 = tangent.x;
    matrix._12 = tangent.y;
    matrix._13 = tangent.z;
    matrix._21 = normal.x;
    matrix._22 = normal.y;
    matrix._23 = normal.z;
    matrix._31 = bitangent.x;
    matrix._32 = bitangent.y;
    matrix._33 = bitangent.z;
    return matrix;
}

D3DMATRIX placement_matrix(const StaticPlacement& placement) {
    auto matrix = multiply_matrix(
        multiply_matrix(rotation_y_matrix(placement.yaw), rotation_x_matrix(placement.pitch)),
        rotation_z_matrix(placement.roll));
    matrix._41 = placement.x;
    matrix._42 = placement.y;
    matrix._43 = placement.z;
    return matrix;
}

D3DMATRIX look_at_rh_matrix(Vec3 eye, Vec3 at, Vec3 up) {
    const Vec3 zaxis = normalize(Vec3{eye.x - at.x, eye.y - at.y, eye.z - at.z});
    const Vec3 xaxis = normalize(cross(up, zaxis));
    const Vec3 yaxis = cross(zaxis, xaxis);

    D3DMATRIX matrix = identity_matrix();
    matrix._11 = xaxis.x;
    matrix._12 = yaxis.x;
    matrix._13 = zaxis.x;
    matrix._21 = xaxis.y;
    matrix._22 = yaxis.y;
    matrix._23 = zaxis.y;
    matrix._31 = xaxis.z;
    matrix._32 = yaxis.z;
    matrix._33 = zaxis.z;
    matrix._41 = -dot(xaxis, eye);
    matrix._42 = -dot(yaxis, eye);
    matrix._43 = -dot(zaxis, eye);
    return matrix;
}

D3DMATRIX perspective_fov_rh_matrix(float fov_y, float aspect, float z_near, float z_far) {
    const float y_scale = 1.0f / std::tan(fov_y * 0.5f);
    const float x_scale = y_scale / (std::max)(aspect, 0.001f);
    D3DMATRIX matrix{};
    matrix._11 = x_scale;
    matrix._22 = y_scale;
    matrix._33 = z_far / (z_near - z_far);
    matrix._34 = -1.0f;
    matrix._43 = (z_near * z_far) / (z_near - z_far);
    return matrix;
}

std::wstring hresult_text(const char* action, HRESULT hr) {
    std::wostringstream out;
    out << action << L" failed: 0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

std::string hresult_text_narrow(const char* action, HRESULT hr) {
    std::ostringstream out;
    out << action << " failed: 0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

void assign_error(std::wstring& error, const std::string& text) {
    error.assign(text.begin(), text.end());
}

IDirect3DTexture9* load_dds_texture(IDirect3DDevice9* device, const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    if (data.size() < 128 || bin::u32le(data, 0) != 0x20534444 ||
        bin::u32le(data, 4) != 124 || bin::u32le(data, 76) != 32) {
        throw std::runtime_error("bad DDS file: " + path.string());
    }

    const auto height = bin::u32le(data, 12);
    const auto width = bin::u32le(data, 16);
    const auto mip_count_raw = bin::u32le(data, 28);
    const auto pf_flags = bin::u32le(data, 80);
    const auto fourcc = bin::u32le(data, 84);
    const auto rgb_bit_count = bin::u32le(data, 88);
    const auto r_mask = bin::u32le(data, 92);
    const auto g_mask = bin::u32le(data, 96);
    const auto b_mask = bin::u32le(data, 100);
    const auto a_mask = bin::u32le(data, 104);
    if (width == 0 || height == 0) {
        throw std::runtime_error("empty DDS texture: " + path.string());
    }

    D3DFORMAT format = D3DFMT_UNKNOWN;
    std::uint32_t block_bytes = 0;
    std::uint32_t source_pixel_bytes = 0;
    bool expand_rgb24 = false;
    if ((pf_flags & 0x4) != 0) {
        if (fourcc == 0x31545844) {
            format = D3DFMT_DXT1;
            block_bytes = 8;
        } else if (fourcc == 0x33545844) {
            format = D3DFMT_DXT3;
            block_bytes = 16;
        } else if (fourcc == 0x35545844) {
            format = D3DFMT_DXT5;
            block_bytes = 16;
        }
    } else if ((pf_flags & 0x40U) != 0 && rgb_bit_count == 32 &&
               r_mask == 0x00ff0000U && g_mask == 0x0000ff00U && b_mask == 0x000000ffU &&
               ((pf_flags & 0x1U) == 0 || a_mask == 0xff000000U)) {
        format = D3DFMT_A8R8G8B8;
        source_pixel_bytes = 4;
    } else if ((pf_flags & 0x40U) != 0 && rgb_bit_count == 16 &&
               r_mask == 0x0000f800U && g_mask == 0x000007e0U && b_mask == 0x0000001fU && a_mask == 0) {
        format = D3DFMT_R5G6B5;
        source_pixel_bytes = 2;
    } else if ((pf_flags & 0x40U) != 0 && rgb_bit_count == 24 &&
               r_mask == 0x00ff0000U && g_mask == 0x0000ff00U && b_mask == 0x000000ffU && a_mask == 0) {
        format = D3DFMT_A8R8G8B8;
        source_pixel_bytes = 3;
        expand_rgb24 = true;
    }
    if (format == D3DFMT_UNKNOWN) {
        throw std::runtime_error("unsupported DDS texture format: " + path.string());
    }

    const UINT mip_count = static_cast<UINT>((std::max)(std::uint32_t{1}, mip_count_raw));
    IDirect3DTexture9* texture = nullptr;
    HRESULT hr = device->CreateTexture(width, height, mip_count, 0, format, D3DPOOL_MANAGED, &texture, nullptr);
    if (FAILED(hr)) {
        throw std::runtime_error(hresult_text_narrow("CreateTexture", hr));
    }

    std::size_t cursor = 128;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (UINT level = 0; level < mip_count; ++level) {
        std::size_t source_pitch = 0;
        std::size_t source_rows = 0;
        if (block_bytes != 0) {
            source_pitch = static_cast<std::size_t>((std::max)(std::uint32_t{1}, (level_width + 3) / 4)) * block_bytes;
            source_rows = (std::max)(std::uint32_t{1}, (level_height + 3) / 4);
        } else {
            source_pitch = static_cast<std::size_t>(level_width) * source_pixel_bytes;
            source_rows = level_height;
        }
        const std::size_t source_bytes = source_pitch * source_rows;
        bin::require_range(data, cursor, source_bytes, "DDS mip data");
        D3DLOCKED_RECT locked{};
        hr = texture->LockRect(level, &locked, nullptr, 0);
        if (FAILED(hr)) {
            release_com(texture);
            throw std::runtime_error(hresult_text_narrow("Texture::LockRect", hr));
        }
        for (std::size_t row = 0; row < source_rows; ++row) {
            auto* dest = static_cast<std::uint8_t*>(locked.pBits) + row * locked.Pitch;
            const auto* source = data.data() + cursor + row * source_pitch;
            if (expand_rgb24) {
                for (std::size_t column = 0; column < level_width; ++column) {
                    dest[column * 4] = source[column * 3];
                    dest[column * 4 + 1] = source[column * 3 + 1];
                    dest[column * 4 + 2] = source[column * 3 + 2];
                    dest[column * 4 + 3] = 0xff;
                }
            } else {
                std::memcpy(dest, source, source_pitch);
            }
        }
        texture->UnlockRect(level);
        cursor += source_bytes;
        level_width = (std::max)(std::uint32_t{1}, level_width / 2);
        level_height = (std::max)(std::uint32_t{1}, level_height / 2);
    }
    return texture;
}

IDirect3DTexture9* load_mtx_texture(IDirect3DDevice9* device, const std::filesystem::path& path) {
    const auto data = bin::read_file(path);
    if (data.size() < 32 || bin::u32le(data, 0) != 0x6d786554) {
        throw std::runtime_error("bad MTX file: " + path.string());
    }
    const auto width = bin::u32le(data, 4);
    const auto height = bin::u32le(data, 8);
    const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * sizeof(std::uint16_t);
    bin::require_range(data, 32, pixel_bytes, "MTX pixels");
    if (data.size() != 32 + pixel_bytes) {
        throw std::runtime_error("unexpected MTX size: " + path.string());
    }

    IDirect3DTexture9* texture = nullptr;
    HRESULT hr = device->CreateTexture(width, height, 1, 0, D3DFMT_A4R4G4B4, D3DPOOL_MANAGED, &texture, nullptr);
    if (FAILED(hr)) {
        throw std::runtime_error(hresult_text_narrow("CreateTexture MTX", hr));
    }
    D3DLOCKED_RECT locked{};
    hr = texture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) {
        release_com(texture);
        throw std::runtime_error(hresult_text_narrow("MTX Texture::LockRect", hr));
    }
    const std::size_t source_pitch = static_cast<std::size_t>(width) * sizeof(std::uint16_t);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(
            static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch,
            data.data() + 32 + static_cast<std::size_t>(row) * source_pitch,
            source_pitch);
    }
    texture->UnlockRect(0);
    return texture;
}

std::string map_tile_stem(const bin::ByteBuffer& map, std::size_t record_offset) {
    bin::require_range(map, record_offset, kMapRecordBytes, "map.bin record");
    std::size_t length = 0;
    while (length < 20 && map[record_offset + length] != 0) {
        ++length;
    }
    std::string name(reinterpret_cast<const char*>(map.data() + record_offset), length);
    if (name.empty()) {
        throw std::runtime_error("map.bin contains an empty tile name");
    }
    if (name == "FILL_EMPT" || name == "fill_empt") {
        return "fill_empt_00";
    }
    std::ostringstream out;
    out << name << "_" << static_cast<int>(map[record_offset + 20]) << static_cast<int>(map[record_offset + 21]);
    return out.str();
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string narrow_ascii(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (const auto ch : value) {
        if (ch < 0 || ch > 0x7f) {
            throw std::runtime_error("Lua world asset name is not ASCII");
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

} // namespace

struct GameWorldScene::Impl {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* overlay_texture = nullptr;
    IDirect3DTexture9* terrain_microtexture = nullptr;
    IDirect3DTexture9* sky_texture = nullptr;
    IDirect3DTexture9* water_texture = nullptr;
    // Planar-reflection render target (the original's 256x256 reflection texture
    // DAT_04e78d94): the scene is re-rendered mirrored about the water plane into
    // this RT, then projected onto the water surface.
    static constexpr UINT kReflectionSize = 256;
    IDirect3DTexture9* reflection_texture = nullptr;
    IDirect3DSurface9* reflection_surface = nullptr;
    IDirect3DSurface9* reflection_depth = nullptr;
    bool rendering_reflection = false;  // true while drawing into the reflection RT
    IDirect3DVertexBuffer9* player_vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* player_index_buffer = nullptr;
    // Programmable-shader path (ported from the original Shaders\Vertex|Pixel\*.vsc/.psc).
    IDirect3DVertexDeclaration9* world_decl = nullptr;
    IDirect3DVertexShader9* base_vs = nullptr;
    IDirect3DPixelShader9* base_ps = nullptr;
    IDirect3DPixelShader9* debug_ps = nullptr;  // visualises texcoord (debug_tc.psc)
    std::unordered_map<std::string, int> base_vs_consts;
    bool world_shaders_ready = false;
    D3DMATRIX view_matrix{};
    D3DMATRIX projection_matrix{};
    D3DMATRIX view_projection_matrix{};
    D3DPRESENT_PARAMETERS present{};
    std::filesystem::path root_path;
    LuaGameWindowConfig config;
    std::unordered_map<std::wstring, std::unique_ptr<TerrainResource>> resources;
    std::vector<TerrainInstance> instances;
    std::unordered_map<std::string, std::unique_ptr<StaticModelResource>> static_resources;
    std::vector<StaticPlacement> static_placements;
    std::vector<StaticInstance> static_instances;
    std::vector<GrassInstance> grass_instances;
    std::unordered_map<int, std::vector<std::uint8_t>> grass_maps;
    std::unordered_set<std::uint64_t> grass_cells;
    std::vector<PlayerBatch> player_batches;
    UINT player_vertex_count = 0;
    // Animated first-person body.
    SkinnedCharacterModel player_model;
    std::vector<float> player_skin_scratch;
    std::vector<WorldVertex> player_vertex_scratch;
    std::size_t player_action = kPlayerIdleAction;
    float player_anim_time = 0.0f;
    int player_head_bone = -1;
    bool player_eye_valid = false;
    bool player_eye_initialized = false;
    bool player_walking = false;
    float player_live_crown_y = 0.0f;   // current frame's head-top world y
    float player_locked_crown_y = 0.0f; // baseline head-top (stable idle eye)
    float player_body_shift = 0.0f; // eased backward offset while moving
    float player_eye_local_x = 0.0f;
    float player_eye_local_y = 0.0f;
    float player_eye_local_z = 0.0f;
    float spawn_x = 0.0f;
    float spawn_y = 0.0f;
    float velocity_y = 0.0f; // vertical velocity for jump/gravity (world +y down)
    float velocity_x = 0.0f; // horizontal velocity (world space) — kept airborne so a jump carries momentum
    float velocity_z = 0.0f;
    bool grounded = true;
    float spawn_z = 0.0f;
    float spawn_angle = 0.0f;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    Vec3 camera_eye{};     // last frame's eye/target (for mirroring in the reflection pass)
    Vec3 camera_target{};
    int terrain_center_row = -1;
    int terrain_center_column = -1;
    int grass_center_x = (std::numeric_limits<int>::min)();
    int grass_center_z = (std::numeric_limits<int>::min)();
    float grass_anchor_x = 0.0f;
    float grass_anchor_z = 0.0f;
    float elapsed_seconds = 0.0f;
    float game_time_fraction = 0.0f;
    int environment_clear_red = 0;
    int environment_clear_green = 0;
    int environment_clear_blue = 0;
    int environment_ambient_red = 110;
    int environment_ambient_green = 110;
    int environment_ambient_blue = 110;
    int environment_sun_red = 255;
    int environment_sun_green = 245;
    int environment_sun_blue = 224;
    int environment_cloud_red = 200;
    int environment_cloud_green = 200;
    int environment_cloud_blue = 200;
    bool grass_anchor_valid = false;
    int overlay_width = 0;
    int overlay_height = 0;
    bool initialized = false;

    ~Impl() {
        release();
    }

    void release() {
        release_com(overlay_texture);
        release_com(terrain_microtexture);
        release_com(sky_texture);
        release_com(water_texture);
        release_com(reflection_depth);
        release_com(reflection_surface);
        release_com(reflection_texture);
        release_com(base_vs);
        release_com(base_ps);
        release_com(debug_ps);
        release_com(world_decl);
        world_shaders_ready = false;
        for (auto& batch : player_batches) {
            release_com(batch.texture);
        }
        player_batches.clear();
        release_com(player_index_buffer);
        release_com(player_vertex_buffer);
        for (auto& [_, resource] : resources) {
            release_com(resource->texture);
            release_com(resource->index_buffer);
            release_com(resource->vertex_buffer);
            release_com(resource->water_index_buffer);
            release_com(resource->water_vertex_buffer);
        }
        for (auto& [_, resource] : static_resources) {
            for (auto& batch : resource->batches) {
                release_com(batch.texture);
            }
            release_com(resource->index_buffer);
            release_com(resource->vertex_buffer);
        }
        grass_instances.clear();
        grass_cells.clear();
        grass_maps.clear();
        static_instances.clear();
        static_placements.clear();
        static_resources.clear();
        instances.clear();
        resources.clear();
        release_com(device);
        release_com(d3d);
        initialized = false;
    }

    // Resolve the local skinned frame buffer for the current action/time and
    // upload it into the player vertex buffer. Also caches the head-bone
    // position so the first-person camera can ride at eye height.
    void skin_player_frame() {
        if (!player_model.valid() || !player_vertex_buffer) {
            return;
        }
        const std::size_t action = player_action < player_model.action_count()
            ? player_action
            : kPlayerIdleAction;
        const std::size_t action_start = player_model.action_frame_start(action);
        const std::size_t action_frames = player_model.action_frame_count(action);
        if (action_frames == 0) {
            return;
        }
        const std::size_t local_frame =
            static_cast<std::size_t>(player_anim_time / kPlayerAnimSecondsPerFrame) % action_frames;
        const std::size_t frame = action_start + local_frame;

        try {
            skin_frame(player_model, frame, player_skin_scratch);
        } catch (...) {
            return;
        }

        const std::size_t vertex_count = player_skin_scratch.size() / 8;
        player_vertex_scratch.resize(vertex_count);
        float crown_world_y = 0.0f; // smallest y == highest point (world +y down)
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const float* s = player_skin_scratch.data() + i * 8;
            // skin_frame emits character-select space (+y up); the world uses
            // +y down, so flip y on both position and normal, matching the
            // original static export path.
            const float wy = -s[1];
            if (i == 0 || wy < crown_world_y) {
                crown_world_y = wy;
            }
            player_vertex_scratch[i] = WorldVertex{
                s[0], wy, s[2],
                s[3], -s[4], s[5],
                0xffffffff,
                s[6], s[7],
                s[6], s[7],
            };
        }

        const UINT vertex_bytes = static_cast<UINT>(vertex_count * sizeof(WorldVertex));
        void* vertex_data = nullptr;
        if (SUCCEEDED(player_vertex_buffer->Lock(0, vertex_bytes, &vertex_data, 0))) {
            std::memcpy(vertex_data, player_vertex_scratch.data(), vertex_bytes);
            player_vertex_buffer->Unlock();
        }

        // Track the head-top each frame; the eye rides just below it.
        player_live_crown_y = crown_world_y;
        // Lock the baseline eye height to the first skinned frame so idle body
        // motion (breathing) does not bob the camera. The walking up/down bob is
        // added in update_view_projection from the live head-top deviation.
        if (!player_eye_initialized) {
            player_locked_crown_y = crown_world_y;
            player_eye_local_x = 0.0f;
            player_eye_local_y = crown_world_y + kEyeBelowCrownWorld;
            player_eye_local_z = kEyeForwardModel;
            player_eye_valid = true;
            player_eye_initialized = true;
        }
    }

    void load_player_model(const SkinnedCharacterModel& model) {
        if (!model.valid()) {
            throw std::runtime_error("selected player skinned model is empty");
        }
        player_model = model;
        player_head_bone = player_model.bone_index("head1");
        if (player_head_bone < 0) {
            player_head_bone = player_model.bone_index("head");
        }
        player_action = kPlayerIdleAction;
        player_anim_time = 0.0f;

        for (const auto& source : player_model.batches) {
            if (source.index_count < 3 ||
                source.start_index > player_model.indices.size() ||
                source.index_count > player_model.indices.size() - source.start_index) {
                throw std::runtime_error("selected player model contains an invalid material batch");
            }
            if (!std::filesystem::exists(source.texture_path)) {
                throw std::runtime_error("selected player texture is missing: " + source.texture_path.string());
            }
            player_batches.push_back(PlayerBatch{
                source.start_index,
                source.index_count,
                load_dds_texture(device, source.texture_path),
                source.is_head,
            });
        }

        player_vertex_count = static_cast<UINT>(player_model.sources.size());
        const UINT vertex_bytes = static_cast<UINT>(player_model.sources.size() * sizeof(WorldVertex));
        HRESULT hr = device->CreateVertexBuffer(
            vertex_bytes,
            0,
            kWorldVertexFvf,
            D3DPOOL_MANAGED,
            &player_vertex_buffer,
            nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("CreateVertexBuffer player", hr));
        }

        const UINT index_bytes = static_cast<UINT>(player_model.indices.size() * sizeof(std::uint16_t));
        hr = device->CreateIndexBuffer(
            index_bytes,
            0,
            D3DFMT_INDEX16,
            D3DPOOL_MANAGED,
            &player_index_buffer,
            nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("CreateIndexBuffer player", hr));
        }
        void* index_data = nullptr;
        hr = player_index_buffer->Lock(0, index_bytes, &index_data, 0);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("PlayerIndexBuffer::Lock", hr));
        }
        std::memcpy(index_data, player_model.indices.data(), index_bytes);
        player_index_buffer->Unlock();

        // Skin the first frame so the body and eye height are valid immediately.
        skin_player_frame();
    }

    void update_player_animation(float delta_seconds, bool moving, bool running) {
        if (!player_model.valid()) {
            return;
        }
        // Action by movement state, using SKL indices carried on the model
        // (read from the original params\<model>.cfg: FREE/WALK/RUN). Until the
        // per-model params reader is wired these default to idle.
        const std::size_t desired = !moving
            ? static_cast<std::size_t>(player_model.anim_idle)
            : (running ? static_cast<std::size_t>(player_model.anim_run)
                       : static_cast<std::size_t>(player_model.anim_walk));
        if (desired != player_action) {
            player_action = desired;
            player_anim_time = 0.0f; // restart the cycle when the action changes
        }
        player_anim_time += (std::max)(0.0f, delta_seconds);

        // Snap the body's backward offset on/off with movement (no easing).
        player_body_shift = moving ? kMoveBodyBackShift : 0.0f;
        player_walking = moving;

        skin_player_frame();
    }

    RECT client_rect() const {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        rc.right = (std::max)(rc.right, rc.left + 1);
        rc.bottom = (std::max)(rc.bottom, rc.top + 1);
        return rc;
    }

    void fill_present_parameters() {
        const RECT rc = client_rect();
        std::memset(&present, 0, sizeof(present));
        present.BackBufferWidth = static_cast<UINT>(rc.right - rc.left);
        present.BackBufferHeight = static_cast<UINT>(rc.bottom - rc.top);
        present.BackBufferFormat = D3DFMT_UNKNOWN;
        present.BackBufferCount = 1;
        present.MultiSampleType = D3DMULTISAMPLE_NONE;
        present.SwapEffect = D3DSWAPEFFECT_DISCARD;
        present.hDeviceWindow = hwnd;
        present.Windowed = TRUE;
        present.EnableAutoDepthStencil = TRUE;
        present.AutoDepthStencilFormat = D3DFMT_D24S8;
        present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    bool create_device(std::wstring& error) {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d) {
            error = L"Direct3DCreate9 failed";
            return false;
        }
        fill_present_parameters();
        const DWORD flags[] = {D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING};
        const D3DFORMAT depths[] = {D3DFMT_D24S8, D3DFMT_D16};
        HRESULT last_hr = E_FAIL;
        for (const auto depth : depths) {
            present.AutoDepthStencilFormat = depth;
            for (const auto flag : flags) {
                last_hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flag, &present, &device);
                if (SUCCEEDED(last_hr)) {
                    return true;
                }
            }
        }
        error = hresult_text("CreateDevice", last_hr);
        return false;
    }

    // (Re)create the planar-reflection render target + its depth buffer. Both are
    // D3DPOOL_DEFAULT, so they must be released before a device Reset and remade
    // after. Failure leaves reflection_texture null → water falls back to flat.
    void create_reflection_target() {
        release_com(reflection_depth);
        release_com(reflection_surface);
        release_com(reflection_texture);
        if (!device) {
            return;
        }
        if (FAILED(device->CreateTexture(kReflectionSize, kReflectionSize, 1, D3DUSAGE_RENDERTARGET,
                                         D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &reflection_texture, nullptr))) {
            reflection_texture = nullptr;
            return;
        }
        reflection_texture->GetSurfaceLevel(0, &reflection_surface);
        if (FAILED(device->CreateDepthStencilSurface(kReflectionSize, kReflectionSize,
                                                     present.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE,
                                                     0, TRUE, &reflection_depth, nullptr))) {
            reflection_depth = nullptr;
        }
    }

    std::filesystem::path resolve_tile_path(const std::string& stem) const {
        for (const auto& dir : config.landscape_dirs) {
            const auto path = root_path / dir / (stem + ".lnd");
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
        throw std::runtime_error("required landscape tile is missing: " + stem + ".lnd");
    }

    std::filesystem::path resolve_model_path(const std::string& model_name) const {
        for (const auto& dir : config.model_dirs) {
            const auto path = root_path / dir / (model_name + ".mdl");
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
        throw std::runtime_error("required static model is missing: " + model_name + ".mdl");
    }

    std::filesystem::path resolve_model_texture_path(
        const std::filesystem::path& model_path,
        const std::string& material_name) const {
        const auto texture_name = lowercase_ascii(material_name) + ".dds";
        const auto local_path = model_path.parent_path() / "textures" / texture_name;
        if (std::filesystem::exists(local_path)) {
            return local_path;
        }
        for (const auto& dir : config.model_dirs) {
            const auto path = root_path / dir / "textures" / texture_name;
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
        throw std::runtime_error(
            "required static model texture is missing: " + material_name + ".dds for " + model_path.string());
    }

    void load_static_placements() {
        static_placements.clear();
        for (const auto& configured_dir : config.static_object_dirs) {
            const auto dir = root_path / configured_dir;
            if (!std::filesystem::is_directory(dir)) {
                throw std::runtime_error("required static object directory is missing: " + dir.string());
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const auto extension = lowercase_ascii(entry.path().extension().string());
                if (extension != ".mbd" && extension != ".mb") {
                    continue;
                }
                const auto data = bin::read_file(entry.path());
                bin::require_range(data, 0, 4, "MBD header");
                const auto count = static_cast<std::size_t>(bin::u16le(data, 0));
                if (bin::u16le(data, 2) != 0) {
                    throw std::runtime_error("invalid MBD header: " + entry.path().string());
                }
                constexpr std::size_t record_bytes = 44;
                bin::require_range(data, 4, count * record_bytes, "MBD object records");
                for (std::size_t i = 0; i < count; ++i) {
                    const std::size_t offset = 4 + i * record_bytes;
                    std::size_t name_length = 0;
                    while (name_length < 20 && data[offset + name_length] != 0) {
                        ++name_length;
                    }
                    std::string model_name(
                        reinterpret_cast<const char*>(data.data() + offset),
                        name_length);
                    if (model_name.empty() || lowercase_ascii(model_name) == "empty") {
                        continue;
                    }
                    static_placements.push_back(StaticPlacement{
                        std::move(model_name),
                        bin::f32le(data, offset + 20),
                        bin::f32le(data, offset + 24),
                        bin::f32le(data, offset + 28),
                        bin::f32le(data, offset + 32),
                        bin::f32le(data, offset + 36),
                        bin::f32le(data, offset + 40),
                    });
                }
            }
        }
        if (static_placements.empty()) {
            throw std::runtime_error("static object directories contain no MBD placements");
        }
    }

    std::unique_ptr<StaticModelResource> load_static_model_resource(
        const std::string& model_name,
        const std::filesystem::path& model_path) {
        const auto mesh = sphere::model::load_mdl_mesh(model_path);
        if (mesh.vertices.empty() || mesh.triangles.empty() || mesh.surfaces.empty() || mesh.info.materials.empty()) {
            throw std::runtime_error("static model has no renderable geometry: " + model_path.string());
        }

        std::vector<WorldVertex> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const auto& source : mesh.vertices) {
            const auto normal = normalize(Vec3{source.nx, source.ny, source.nz});
            vertices.push_back(WorldVertex{
                source.x,
                source.y,
                source.z,
                normal.x,
                normal.y,
                normal.z,
                0xffffffff,
                source.u,
                source.v,
                source.u,
                source.v,
            });
        }

        std::vector<std::vector<std::uint16_t>> indices_by_material(mesh.info.materials.size());
        // Collision uses only solid triangles: the MDL triangle flag 0x100 marks
        // pass-through geometry (foliage/canopy — whole bushes are 0x100, tree
        // leaves are 0x100 while trunks are 0, solid props like stone/castle are
        // all 0). Colliding against every triangle put invisible walls in leaves.
        std::vector<std::uint16_t> collision_idx;
        for (const auto& surface : mesh.surfaces) {
            if (surface.first_triangle_index < 0 || surface.triangle_count < 0 ||
                surface.first_vertex_index < 0 || surface.vertex_count < 0) {
                throw std::runtime_error("static model has negative surface ranges: " + model_path.string());
            }
            const auto material = static_cast<std::size_t>(surface.texture_index);
            const auto first_triangle = static_cast<std::size_t>(surface.first_triangle_index);
            const auto triangle_count = static_cast<std::size_t>(surface.triangle_count);
            const auto first_vertex = static_cast<std::size_t>(surface.first_vertex_index);
            const auto vertex_count = static_cast<std::size_t>(surface.vertex_count);
            if (material >= indices_by_material.size() ||
                first_triangle > mesh.triangles.size() || triangle_count > mesh.triangles.size() - first_triangle ||
                first_vertex > mesh.vertices.size() || vertex_count > mesh.vertices.size() - first_vertex) {
                throw std::runtime_error("static model surface range is invalid: " + model_path.string());
            }
            auto& indices = indices_by_material[material];
            for (std::size_t i = 0; i < triangle_count; ++i) {
                const auto& triangle = mesh.triangles[first_triangle + i];
                if (triangle.a >= vertex_count || triangle.b >= vertex_count || triangle.c >= vertex_count) {
                    throw std::runtime_error("static model triangle range is invalid: " + model_path.string());
                }
                const auto ia = static_cast<std::uint16_t>(first_vertex + triangle.a);
                const auto ib = static_cast<std::uint16_t>(first_vertex + triangle.b);
                const auto ic = static_cast<std::uint16_t>(first_vertex + triangle.c);
                indices.push_back(ia);
                indices.push_back(ib);
                indices.push_back(ic);
                if ((triangle.flags & 0x100) == 0) {  // solid (collidable) triangle
                    collision_idx.push_back(ia);
                    collision_idx.push_back(ib);
                    collision_idx.push_back(ic);
                }
            }
        }

        std::vector<std::uint16_t> indices;
        auto resource = std::make_unique<StaticModelResource>();
        resource->vertex_count = static_cast<UINT>(vertices.size());
        resource->bounds_min = Vec3{mesh.bounds.min_x, mesh.bounds.min_y, mesh.bounds.min_z};
        resource->bounds_max = Vec3{mesh.bounds.max_x, mesh.bounds.max_y, mesh.bounds.max_z};
        resource->collision_positions.reserve(mesh.vertices.size());
        for (const auto& vertex : mesh.vertices) {
            resource->collision_positions.push_back(Vec3{vertex.x, vertex.y, vertex.z});
        }
        for (std::size_t material = 0; material < indices_by_material.size(); ++material) {
            auto& material_indices = indices_by_material[material];
            if (material_indices.empty()) {
                continue;
            }
            StaticModelBatch batch;
            batch.start_index = static_cast<UINT>(indices.size());
            batch.index_count = static_cast<UINT>(material_indices.size());
            batch.texture = load_dds_texture(
                device,
                resolve_model_texture_path(model_path, mesh.info.materials[material]));
            indices.insert(indices.end(), material_indices.begin(), material_indices.end());
            resource->batches.push_back(batch);
        }
        // Collision excludes pass-through (flag 0x100) triangles — see above.
        resource->collision_indices = std::move(collision_idx);
        if (indices.empty() || resource->batches.empty()) {
            throw std::runtime_error("static model has no material batches: " + model_name);
        }

        const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(WorldVertex));
        HRESULT hr = device->CreateVertexBuffer(
            vertex_bytes,
            0,
            kWorldVertexFvf,
            D3DPOOL_MANAGED,
            &resource->vertex_buffer,
            nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("CreateVertexBuffer static model", hr));
        }
        void* vertex_data = nullptr;
        hr = resource->vertex_buffer->Lock(0, vertex_bytes, &vertex_data, 0);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("StaticModelVertexBuffer::Lock", hr));
        }
        std::memcpy(vertex_data, vertices.data(), vertex_bytes);
        resource->vertex_buffer->Unlock();

        const UINT index_bytes = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        hr = device->CreateIndexBuffer(
            index_bytes,
            0,
            D3DFMT_INDEX16,
            D3DPOOL_MANAGED,
            &resource->index_buffer,
            nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("CreateIndexBuffer static model", hr));
        }
        void* index_data = nullptr;
        hr = resource->index_buffer->Lock(0, index_bytes, &index_data, 0);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("StaticModelIndexBuffer::Lock", hr));
        }
        std::memcpy(index_data, indices.data(), index_bytes);
        resource->index_buffer->Unlock();
        return resource;
    }

    void load_visible_static_objects() {
        static_instances.clear();
        const float radius_squared = config.static_object_radius * config.static_object_radius;
        for (const auto& placement : static_placements) {
            const float dx = placement.x - spawn_x;
            const float dz = placement.z - spawn_z;
            if (dx * dx + dz * dz > radius_squared) {
                continue;
            }
            const auto key = lowercase_ascii(placement.model_name);
            auto it = static_resources.find(key);
            if (it == static_resources.end()) {
                const auto model_path = resolve_model_path(placement.model_name);
                it = static_resources.emplace(
                    key,
                    load_static_model_resource(placement.model_name, model_path)).first;
            }
            const auto world = placement_matrix(placement);
            Vec3 bounds_min{
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
            };
            Vec3 bounds_max{
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
            };
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 local{
                    (corner & 1) ? it->second->bounds_max.x : it->second->bounds_min.x,
                    (corner & 2) ? it->second->bounds_max.y : it->second->bounds_min.y,
                    (corner & 4) ? it->second->bounds_max.z : it->second->bounds_min.z,
                };
                const Vec3 point = transform_point(local, world);
                bounds_min.x = (std::min)(bounds_min.x, point.x);
                bounds_min.y = (std::min)(bounds_min.y, point.y);
                bounds_min.z = (std::min)(bounds_min.z, point.z);
                bounds_max.x = (std::max)(bounds_max.x, point.x);
                bounds_max.y = (std::max)(bounds_max.y, point.y);
                bounds_max.z = (std::max)(bounds_max.z, point.z);
            }
            static_instances.push_back(StaticInstance{it->second.get(), world, bounds_min, bounds_max});
        }
    }

    const std::vector<std::uint8_t>& load_grass_map(int chunk_x, int chunk_z) {
        const int key = chunk_z * config.grassmap_grid_size + chunk_x;
        const auto cached = grass_maps.find(key);
        if (cached != grass_maps.end()) {
            return cached->second;
        }
        std::wostringstream name;
        name << L"grassmap_" << std::setw(2) << std::setfill(L'0') << chunk_x
             << L"_" << std::setw(2) << std::setfill(L'0') << chunk_z << L".bin";
        const auto path = root_path / config.grassmap_dir / name.str();
        const auto data = bin::read_file(path);
        const std::size_t expected =
            static_cast<std::size_t>(config.grassmap_tile_resolution) *
            static_cast<std::size_t>(config.grassmap_tile_resolution);
        if (data.size() != expected) {
            throw std::runtime_error("invalid grass map size: " + path.string());
        }
        return grass_maps.emplace(key, data).first->second;
    }

    std::uint8_t grass_type_at(float world_x, float world_z) {
        const int map_x = static_cast<int>(std::floor(
            (config.grassmap_world_offset_x + static_cast<float>(config.grassmap_world_sign_x) * world_x) *
            config.grassmap_world_scale));
        const int map_z = static_cast<int>(std::floor(
            (config.grassmap_world_offset_z + static_cast<float>(config.grassmap_world_sign_z) * world_z) *
            config.grassmap_world_scale));
        const int world_resolution = config.grassmap_grid_size * config.grassmap_tile_resolution;
        if (map_x < 0 || map_z < 0 || map_x >= world_resolution || map_z >= world_resolution) {
            return 0;
        }
        const int chunk_x = map_x / config.grassmap_tile_resolution;
        const int chunk_z = map_z / config.grassmap_tile_resolution;
        const int local_x = map_x % config.grassmap_tile_resolution;
        const int local_z = map_z % config.grassmap_tile_resolution;
        const auto& map = load_grass_map(chunk_x, chunk_z);
        std::uint8_t type =
            map[static_cast<std::size_t>(local_z) * config.grassmap_tile_resolution + local_x] & 0x0f;
        if (type != 0 &&
            spawn_y > config.grass_highland_min_y &&
            spawn_y < config.grass_highland_max_y) {
            type = static_cast<std::uint8_t>(type + config.grass_highland_pattern_offset);
        }
        return type;
    }

    void load_visible_grass() {
        if (config.grass_quality <= 0) {
            grass_instances.clear();
            grass_cells.clear();
            return;
        }
        if (config.grass_detail_models.empty()) {
            throw std::runtime_error("grass_detail_models is empty");
        }
        if (config.grass_sample_offsets.empty()) {
            throw std::runtime_error("grass_sample_offsets is empty");
        }
        const float spacing = config.grass_spacing;
        grass_anchor_x = spawn_x;
        grass_anchor_z = spawn_z;
        grass_anchor_valid = true;
        grass_center_x = static_cast<int>(std::floor(grass_anchor_x / spacing));
        grass_center_z = static_cast<int>(std::floor(grass_anchor_z / spacing));
        const float generation_radius = config.grass_radius + config.grass_generation_margin;
        const int cell_radius = static_cast<int>(std::ceil(generation_radius / spacing)) + 1;
        const float cell_selection_radius = generation_radius + spacing;
        const float cell_selection_radius_squared = cell_selection_radius * cell_selection_radius;
        auto cell_key = [](int x, int z) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                   static_cast<std::uint32_t>(z);
        };
        std::unordered_set<std::uint64_t> target_cells;
        for (int cell_x = grass_center_x - cell_radius; cell_x <= grass_center_x + cell_radius; ++cell_x) {
            for (int cell_z = grass_center_z - cell_radius; cell_z <= grass_center_z + cell_radius; ++cell_z) {
                const float x = static_cast<float>(cell_x) * spacing;
                const float z = static_cast<float>(cell_z) * spacing;
                const float dx = x + spacing * 0.5f - grass_anchor_x;
                const float dz = z + spacing * 0.5f - grass_anchor_z;
                if (dx * dx + dz * dz <= cell_selection_radius_squared) {
                    target_cells.insert(cell_key(cell_x, cell_z));
                }
            }
        }
        std::erase_if(grass_instances, [&target_cells, &cell_key](const GrassInstance& instance) {
            return !target_cells.contains(cell_key(instance.cell_x, instance.cell_z));
        });
        std::erase_if(grass_cells, [&target_cells](std::uint64_t key) {
            return !target_cells.contains(key);
        });

        for (int cell_x = grass_center_x - cell_radius; cell_x <= grass_center_x + cell_radius; ++cell_x) {
            for (int cell_z = grass_center_z - cell_radius; cell_z <= grass_center_z + cell_radius; ++cell_z) {
                const auto key = cell_key(cell_x, cell_z);
                if (!target_cells.contains(key) || grass_cells.contains(key)) {
                    continue;
                }
                grass_cells.insert(key);
                const float x = static_cast<float>(cell_x) * spacing;
                const float z = static_cast<float>(cell_z) * spacing;
                // Track the cell outcome so flowers can be scattered only when all
                // samples were flat grass and none produced detail (FUN_0047a150:
                // local_a8 == sample_count && local_9d == 0).
                int flat_sample_count = 0;
                bool any_detail = false;
                int flower_type = 0;
                for (std::size_t sample_index = 0; sample_index < config.grass_sample_offsets.size(); ++sample_index) {
                    const auto& sample_offset = config.grass_sample_offsets[sample_index];
                    const float sample_x = x + sample_offset.x;
                    const float sample_z = z + sample_offset.z;
                    const auto type = grass_type_at(sample_x, sample_z);
                    if (type == 0 || type >= config.grass_patterns.size()) {
                        continue;
                    }
                    if (config.grass_patterns[type].empty()) {
                        throw std::runtime_error("grass pattern has no models for type " + std::to_string(type));
                    }

                    std::uint32_t random_state =
                        (static_cast<std::uint32_t>(cell_x) * 0x9e3779b9U) ^
                        (static_cast<std::uint32_t>(cell_z) * 0x85ebca6bU) ^
                        (static_cast<std::uint32_t>(sample_index) * 0xc2b2ae35U) ^
                        type;
                    auto next_random = [&random_state]() {
                        random_state ^= random_state << 13;
                        random_state ^= random_state >> 17;
                        random_state ^= random_state << 5;
                        return random_state;
                    };
                    auto unit_random = [&next_random]() {
                        return static_cast<float>(next_random() & 0xffffU) / 65535.0f;
                    };

                    float flat_height = 0.0f;
                    Vec3 flat_normal{};
                    if (flat_grass_surface_at(sample_x, sample_z, flat_height, flat_normal)) {
                        const auto& configured_model =
                            config.grass_patterns[type][next_random() % config.grass_patterns[type].size()];
                        const auto model_name = narrow_ascii(configured_model);
                        const auto key = lowercase_ascii(model_name);
                        auto resource = static_resources.find(key);
                        if (resource == static_resources.end()) {
                            resource = static_resources.emplace(
                                key,
                                load_static_model_resource(model_name, resolve_model_path(model_name))).first;
                        }
                        // Original FUN_0047a150 (flat branch) stores position +
                        // terrain normal + scale 1.0 with NO random yaw — the blade
                        // tuft is only aligned to the ground, not spun.
                        auto world = align_up_matrix(flat_normal);
                        world._41 = sample_x;
                        world._42 = flat_height - resource->second->bounds_max.y;
                        world._43 = sample_z;
                        grass_instances.push_back(GrassInstance{
                            resource->second.get(),
                            world,
                            unit_random() * 2.0f * kPi,
                            0.65f + unit_random() * 0.35f,
                            cell_x,
                            cell_z,
                        });
                        ++flat_sample_count;
                        flower_type = static_cast<int>(type);
                        continue;
                    }

                    const int detail_count = config.grass_detail_count;
                    for (int detail = 0; detail < detail_count; ++detail) {
                        const float jitter = config.grass_spacing * config.grass_jitter_fraction;
                        const float detail_x = sample_x + (unit_random() * 2.0f - 1.0f) * jitter;
                        const float detail_z = sample_z + (unit_random() * 2.0f - 1.0f) * jitter;
                        float height = 0.0f;
                        Vec3 detail_normal{};
                        if (!terrain_surface_at(detail_x, detail_z, height, detail_normal)) {
                            continue;
                        }
                        const auto& configured_model =
                            config.grass_detail_models[next_random() % config.grass_detail_models.size()];
                        const auto model_name = narrow_ascii(configured_model);
                        const auto key = lowercase_ascii(model_name);
                        auto resource = static_resources.find(key);
                        if (resource == static_resources.end()) {
                            const auto model_path = resolve_model_path(model_name);
                            resource = static_resources.emplace(
                                key,
                                load_static_model_resource(model_name, model_path)).first;
                        }
                        // Original detail branch (FUN_0047a150): random jitter +
                        // random scale, but NO random yaw rotation.
                        const float scale =
                            config.grass_scale_min + unit_random() * (config.grass_scale_max - config.grass_scale_min);
                        auto world = scale_matrix(scale);
                        world._41 = detail_x;
                        world._42 = height - resource->second->bounds_max.y * scale;
                        world._43 = detail_z;
                        grass_instances.push_back(GrassInstance{
                            resource->second.get(),
                            world,
                            unit_random() * 2.0f * kPi,
                            0.65f + unit_random() * 0.35f,
                            cell_x,
                            cell_z,
                        });
                        any_detail = true;
                    }
                }

                // FLOWERS (FUN_0047a150 tail): only when every sample produced flat
                // grass and none produced detail. Scatter a random count (0..max-1)
                // of "flower"+suffix models over the cell, each picking a random
                // flower slot 0-4 and skipping it if the pattern has none there.
                const int sample_total = static_cast<int>(config.grass_sample_offsets.size());
                if (flat_sample_count == sample_total && !any_detail && flower_type > 0 &&
                    flower_type < static_cast<int>(config.grass_flower_patterns.size()) &&
                    !config.grass_flower_patterns[static_cast<std::size_t>(flower_type)].empty()) {
                    const auto& flowers = config.grass_flower_patterns[static_cast<std::size_t>(flower_type)];
                    std::uint32_t flower_state =
                        (static_cast<std::uint32_t>(cell_x) * 0x27d4eb2dU) ^
                        (static_cast<std::uint32_t>(cell_z) * 0x165667b1U) ^ 0x9e3779b9U;
                    auto flower_next = [&flower_state]() {
                        flower_state ^= flower_state << 13;
                        flower_state ^= flower_state >> 17;
                        flower_state ^= flower_state << 5;
                        return flower_state;
                    };
                    auto flower_unit = [&flower_next]() {
                        return static_cast<float>(flower_next() & 0xffffU) / 65535.0f;
                    };
                    const int flower_count =
                        static_cast<int>(flower_unit() * static_cast<float>(config.grass_flower_count_max));
                    for (int f = 0; f < flower_count; ++f) {
                        const float flower_x = x + flower_unit() * spacing;
                        const float flower_z = z + flower_unit() * spacing;
                        float flower_h = 0.0f;
                        Vec3 flower_normal{};
                        if (!terrain_surface_at(flower_x, flower_z, flower_h, flower_normal)) {
                            continue;
                        }
                        const int slot = static_cast<int>(flower_unit() * 5.0f);  // native (rand*5)>>15
                        if (slot < 0 || slot >= static_cast<int>(flowers.size())) {
                            continue;  // empty flower slot → no flower this iteration
                        }
                        const auto model_name = narrow_ascii(flowers[static_cast<std::size_t>(slot)]);
                        const auto key2 = lowercase_ascii(model_name);
                        auto resource = static_resources.find(key2);
                        if (resource == static_resources.end()) {
                            resource = static_resources.emplace(
                                key2,
                                load_static_model_resource(model_name, resolve_model_path(model_name))).first;
                        }
                        auto world = align_up_matrix(flower_normal);
                        world._41 = flower_x;
                        world._42 = flower_h - resource->second->bounds_max.y;
                        world._43 = flower_z;
                        grass_instances.push_back(GrassInstance{
                            resource->second.get(),
                            world,
                            flower_unit() * 2.0f * kPi,
                            0.65f + flower_unit() * 0.35f,
                            cell_x,
                            cell_z,
                        });
                    }
                }
            }
        }
    }

    std::unique_ptr<TerrainResource> load_resource(const std::filesystem::path& lnd_path) {
        const auto data = bin::read_file(lnd_path);
        if (data.size() < kLndHeaderBytes) {
            throw std::runtime_error("truncated LND header: " + lnd_path.string());
        }
        const auto vertex_count = static_cast<std::size_t>(bin::u16le(data, 4));
        if (vertex_count == 0 || vertex_count > 65535) {
            throw std::runtime_error("invalid LND vertex count: " + lnd_path.string());
        }
        const std::size_t triangles_offset = kLndHeaderBytes + vertex_count * kLndVertexBytes;
        bin::require_range(data, kLndHeaderBytes, vertex_count * kLndVertexBytes, "LND vertices");
        if (triangles_offset > data.size() || (data.size() - triangles_offset) % kLndTriangleBytes != 0) {
            throw std::runtime_error("invalid LND triangle table: " + lnd_path.string());
        }
        const std::size_t triangle_count = (data.size() - triangles_offset) / kLndTriangleBytes;
        if (triangle_count == 0) {
            throw std::runtime_error("LND contains no triangles: " + lnd_path.string());
        }

        std::vector<WorldVertex> vertices;
        vertices.reserve(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const std::size_t offset = kLndHeaderBytes + i * kLndVertexBytes;
            const auto normal = normalize(Vec3{bin::f32le(data, offset + 12), bin::f32le(data, offset + 16), bin::f32le(data, offset + 20)});
            vertices.push_back(WorldVertex{
                bin::f32le(data, offset),
                bin::f32le(data, offset + 4),
                bin::f32le(data, offset + 8),
                normal.x,
                normal.y,
                normal.z,
                0xffffffff,
                bin::f32le(data, offset + 24),
                bin::f32le(data, offset + 28),
                bin::f32le(data, offset + 32),
                bin::f32le(data, offset + 36),
            });
        }

        std::vector<std::uint16_t> indices;
        indices.reserve(triangle_count * 3);
        for (std::size_t i = 0; i < triangle_count; ++i) {
            const std::size_t offset = triangles_offset + i * kLndTriangleBytes;
            const auto a = bin::u16le(data, offset);
            const auto b = bin::u16le(data, offset + 2);
            const auto c = bin::u16le(data, offset + 4);
            if (a >= vertex_count || b >= vertex_count || c >= vertex_count) {
                throw std::runtime_error("LND triangle references missing vertex: " + lnd_path.string());
            }
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
        }

        auto resource = std::make_unique<TerrainResource>();
        resource->lnd_path = lnd_path;
        resource->vertex_count = static_cast<UINT>(vertices.size());
        resource->index_count = static_cast<UINT>(indices.size());
        resource->positions.reserve(vertices.size());
        for (const auto& vertex : vertices) {
            resource->positions.push_back(Vec3{vertex.x, vertex.y, vertex.z});
        }
        resource->indices = indices;
        auto texture_path = lnd_path;
        texture_path.replace_extension(".dds");
        if (!std::filesystem::exists(texture_path)) {
            throw std::runtime_error("required landscape texture is missing: " + texture_path.string());
        }
        resource->texture = load_dds_texture(device, texture_path);

        const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(WorldVertex));
        HRESULT hr = device->CreateVertexBuffer(vertex_bytes, 0, kWorldVertexFvf, D3DPOOL_MANAGED, &resource->vertex_buffer, nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("CreateVertexBuffer terrain", hr));
        }
        void* vertex_data = nullptr;
        hr = resource->vertex_buffer->Lock(0, vertex_bytes, &vertex_data, 0);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("TerrainVertexBuffer::Lock", hr));
        }
        std::memcpy(vertex_data, vertices.data(), vertex_bytes);
        resource->vertex_buffer->Unlock();

        const UINT index_bytes = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        hr = device->CreateIndexBuffer(index_bytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &resource->index_buffer, nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("CreateIndexBuffer terrain", hr));
        }
        void* index_data = nullptr;
        hr = resource->index_buffer->Lock(0, index_bytes, &index_data, 0);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("TerrainIndexBuffer::Lock", hr));
        }
        std::memcpy(index_data, indices.data(), index_bytes);
        resource->index_buffer->Unlock();
        build_water_mesh(*resource, lnd_path);
        return resource;
    }

    // Build the patch water surface from the matching .wtr (12x12 grid, 2 floats
    // per cell: [0]=water-surface height in patch-local space (>=900 = no water),
    // [1]=0). Emits a quad per cell where all four corners are water. Patch-local
    // X/Z span [0,100] (tile_size); the 12 samples map to that range.
    void build_water_mesh(TerrainResource& resource, const std::filesystem::path& lnd_path) {
        auto wtr_path = lnd_path;
        wtr_path.replace_extension(".wtr");
        if (!std::filesystem::exists(wtr_path)) {
            return;
        }
        const auto data = bin::read_file(wtr_path);
        constexpr int kGrid = 12;
        if (data.size() < static_cast<std::size_t>(kGrid) * kGrid * 2 * 4) {
            return;
        }
        auto height = [&](int r, int c) {
            return bin::f32le(data, (static_cast<std::size_t>(r) * kGrid + c) * 2 * 4);
        };
        // Each cell is [0]=water-surface height, [1]=water mask (int 1 = water,
        // 0 = no water). The mask — NOT a height threshold — decides where water
        // exists: dry cells store height 0.0 OR >=900, and 0.0 would otherwise be
        // read as a surface at world Y 0, spiking the quad ~167 units into the sky.
        auto is_water = [&](int r, int c) {
            return bin::i32le(data, (static_cast<std::size_t>(r) * kGrid + c) * 2 * 4 + 4) != 0;
        };
        const float step = config.tile_size / static_cast<float>(kGrid - 1);  // 12 samples over [0,100]
        std::vector<WorldVertex> verts;
        std::vector<std::uint16_t> idx;
        for (int r = 0; r < kGrid - 1; ++r) {
            for (int c = 0; c < kGrid - 1; ++c) {
                if (!is_water(r, c) || !is_water(r, c + 1) ||
                    !is_water(r + 1, c) || !is_water(r + 1, c + 1)) {
                    continue;  // quad not fully submerged (mask must be set on all 4 corners)
                }
                const float h00 = height(r, c), h01 = height(r, c + 1);
                const float h10 = height(r + 1, c), h11 = height(r + 1, c + 1);
                const auto base = static_cast<std::uint16_t>(verts.size());
                auto push = [&](int rr, int cc, float h) {
                    const float x = cc * step;
                    const float z = rr * step;
                    verts.push_back(WorldVertex{
                        x, h, z,
                        0.0f, -1.0f, 0.0f,  // up (world +Y is down)
                        0xffffffff,
                        x / 12.5f, z / 12.5f, 0.0f, 0.0f});
                };
                push(r, c, h00);
                push(r, c + 1, h01);
                push(r + 1, c, h10);
                push(r + 1, c + 1, h11);
                idx.push_back(base);
                idx.push_back(static_cast<std::uint16_t>(base + 1));
                idx.push_back(static_cast<std::uint16_t>(base + 2));
                idx.push_back(static_cast<std::uint16_t>(base + 2));
                idx.push_back(static_cast<std::uint16_t>(base + 1));
                idx.push_back(static_cast<std::uint16_t>(base + 3));
            }
        }
        if (verts.empty()) {
            return;
        }
        resource.water_vertex_count = static_cast<UINT>(verts.size());
        resource.water_index_count = static_cast<UINT>(idx.size());
        resource.water_height = verts.front().y;  // planar-reflection plane for this patch
        resource.has_water = true;
        resource.water_cpu_verts = verts;  // base verts for per-frame wave displacement
        const UINT vbytes = static_cast<UINT>(verts.size() * sizeof(WorldVertex));
        if (SUCCEEDED(device->CreateVertexBuffer(vbytes, 0, kWorldVertexFvf, D3DPOOL_MANAGED, &resource.water_vertex_buffer, nullptr))) {
            void* p = nullptr;
            if (SUCCEEDED(resource.water_vertex_buffer->Lock(0, vbytes, &p, 0))) {
                std::memcpy(p, verts.data(), vbytes);
                resource.water_vertex_buffer->Unlock();
            }
        }
        const UINT ibytes = static_cast<UINT>(idx.size() * sizeof(std::uint16_t));
        if (SUCCEEDED(device->CreateIndexBuffer(ibytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &resource.water_index_buffer, nullptr))) {
            void* p = nullptr;
            if (SUCCEEDED(resource.water_index_buffer->Lock(0, ibytes, &p, 0))) {
                std::memcpy(p, idx.data(), ibytes);
                resource.water_index_buffer->Unlock();
            }
        }
    }

    void load_visible_terrain() {
        const auto map = bin::read_file(root_path / config.map_file);
        const std::size_t expected_records = static_cast<std::size_t>(config.grid_width) * static_cast<std::size_t>(config.grid_width);
        if (map.size() != expected_records * kMapRecordBytes) {
            throw std::runtime_error("map.bin size does not match configured square grid");
        }

        const int center_row = static_cast<int>(std::floor(spawn_x / config.tile_size)) + config.origin_row;
        const int center_column = config.origin_column - static_cast<int>(std::floor(spawn_z / config.tile_size));
        if (center_row < 0 || center_row >= config.grid_width || center_column < 0 || center_column >= config.grid_width) {
            throw std::runtime_error("spawn coordinates are outside landscape map");
        }

        instances.clear();
        for (int row = center_row - config.visible_radius; row <= center_row + config.visible_radius; ++row) {
            for (int column = center_column - config.visible_radius; column <= center_column + config.visible_radius; ++column) {
                if (row < 0 || row >= config.grid_width || column < 0 || column >= config.grid_width) {
                    continue;
                }
                const std::size_t record = static_cast<std::size_t>(row * config.grid_width + column);
                const auto stem = map_tile_stem(map, record * kMapRecordBytes);
                const auto lnd_path = resolve_tile_path(stem);
                const auto key = lnd_path.wstring();
                auto it = resources.find(key);
                if (it == resources.end()) {
                    it = resources.emplace(key, load_resource(lnd_path)).first;
                }
                instances.push_back(TerrainInstance{
                    it->second.get(),
                    static_cast<float>(row - config.origin_row) * config.tile_size,
                    static_cast<float>(config.origin_column - column) * config.tile_size,
                });
            }
        }
        if (instances.empty()) {
            throw std::runtime_error("no landscape instances were loaded for spawn");
        }
        terrain_center_row = center_row;
        terrain_center_column = center_column;
    }

    // Load the base lit+textured shader pair (Shaders\Vertex\00_00_00_00.vsc =
    // transform + 2 directional lights + ambient with 1.3x overbright;
    // Shaders\Pixel\00_00_00_00_00.psc = texture * saturate(lighting)) and a
    // vertex declaration matching WorldVertex. Falls back to fixed-function if
    // anything fails (world_shaders_ready stays false).
    void load_world_shaders() {
        const auto vs_code = read_binary_file(root_path / "shaders" / "vertex" / "00_00_00_00.vsc");
        const auto ps_code = read_binary_file(root_path / "shaders" / "pixel" / "00_00_00_00_00.psc");
        if (vs_code.empty() || ps_code.empty()) {
            return;
        }
        if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs_code.data()), &base_vs)) ||
            FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps_code.data()), &base_ps))) {
            release_com(base_vs);
            release_com(base_ps);
            return;
        }
        base_vs_consts = parse_shader_constants(vs_code);

        static const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
            {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END()};
        if (FAILED(device->CreateVertexDeclaration(elements, &world_decl))) {
            release_com(base_vs);
            release_com(base_ps);
            release_com(world_decl);
            return;
        }
        world_shaders_ready = true;

        const auto dbg = read_binary_file(root_path / "shaders" / "pixel" / "debug_tc.psc");
        if (!dbg.empty()) {
            device->CreatePixelShader(reinterpret_cast<const DWORD*>(dbg.data()), &debug_ps);
        }
    }

    void set_vs_const(const char* name, const float* data, int vec4_count) {
        const auto it = base_vs_consts.find(name);
        if (it != base_vs_consts.end()) {
            device->SetVertexShaderConstantF(static_cast<UINT>(it->second), data, static_cast<UINT>(vec4_count));
        }
    }

    // Set the per-frame lighting COLOURS (world-independent): directional sun
    // colour + ambient. Light 1 is left dark (single sun). The light DIRECTION
    // is per-object (local space) and set in set_base_world.
    void set_base_light_constants() {
        const float colors[8] = {
            environment_sun_red / 255.0f, environment_sun_green / 255.0f, environment_sun_blue / 255.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f};
        set_vs_const("gDirLightColor", colors, 2);
        const float ambient[4] = {
            environment_ambient_red / 255.0f, environment_ambient_green / 255.0f, environment_ambient_blue / 255.0f, 1.0f};
        set_vs_const("gAmbientColor", ambient, 1);
    }

    // Per-object base-shader constants: world-view-projection (transposed for the
    // dp4-style vertex shader) and the sun direction transformed into the object's
    // LOCAL space (the shader lights against the model-space normal). Local dir =
    // worldToLight * transpose(world 3x3), normalised (handles rotation+scale).
    void set_base_world(const D3DMATRIX& world) {
        const D3DMATRIX wvp = transpose_matrix(multiply_matrix(world, view_projection_matrix));
        set_vs_const("gWorldViewProjection", reinterpret_cast<const float*>(&wvp), 4);

        const float wl[3] = {0.40452f, 0.86683f, -0.52009f};  // normalised world toLight (0.35,0.75,-0.45)
        float local[8] = {
            wl[0] * world._11 + wl[1] * world._12 + wl[2] * world._13,
            wl[0] * world._21 + wl[1] * world._22 + wl[2] * world._23,
            wl[0] * world._31 + wl[1] * world._32 + wl[2] * world._33,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        const float len = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
        if (len > 0.0001f) {
            local[0] /= len;
            local[1] /= len;
            local[2] /= len;
        }
        set_vs_const("gDirLightToLightDirL", local, 2);
    }

    // Switch the device to the base shader pipeline (decl + shaders); call
    // set_base_light_constants once after this, then set_base_world per object.
    void begin_base_shader() {
        device->SetVertexDeclaration(world_decl);
        device->SetVertexShader(base_vs);
        device->SetPixelShader(base_ps);
    }

    // Return to the fixed-function pipeline for passes not yet ported.
    void end_base_shader() {
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
    }

    void configure_render_state() {
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_LIGHTING, TRUE);
        device->SetRenderState(
            D3DRS_AMBIENT,
            D3DCOLOR_XRGB(environment_ambient_red, environment_ambient_green, environment_ambient_blue));
        device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
        device->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetRenderState(D3DRS_FOGENABLE, TRUE);
        device->SetRenderState(
            D3DRS_FOGCOLOR,
            D3DCOLOR_XRGB(environment_clear_red, environment_clear_green, environment_clear_blue));
        // Use PIXEL (table) fog, not vertex fog: vertex fog requires the vertex
        // shader to output oFog, which the ported shaders don't, so shader-drawn
        // geometry (terrain/objects/player) was getting an undefined fog factor
        // and rendering solid fog-blue. Table fog is computed per-pixel from depth
        // and works for both the fixed-function and shader passes.
        device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        DWORD fog_start = 0;
        DWORD fog_end = 0;
        std::memcpy(&fog_start, &config.fog_start, sizeof(fog_start));
        std::memcpy(&fog_end, &config.fog_end, sizeof(fog_end));
        device->SetRenderState(D3DRS_FOGSTART, fog_start);
        device->SetRenderState(D3DRS_FOGEND, fog_end);

        D3DMATERIAL9 material{};
        material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = material.Diffuse.a = 1.0f;
        material.Ambient = material.Diffuse;
        device->SetMaterial(&material);

        D3DLIGHT9 light{};
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse.r = static_cast<float>(environment_sun_red) / 255.0f;
        light.Diffuse.g = static_cast<float>(environment_sun_green) / 255.0f;
        light.Diffuse.b = static_cast<float>(environment_sun_blue) / 255.0f;
        // Ambient comes solely from the global D3DRS_AMBIENT (the data-driven
        // environment ambient). The old fixed 0.35 here double-counted ambient
        // and washed the scene flat.
        light.Ambient.r = light.Ambient.g = light.Ambient.b = 0.0f;
        light.Direction.x = -0.35f;
        light.Direction.y = -0.75f;
        light.Direction.z = 0.45f;
        device->SetLight(0, &light);
        device->LightEnable(0, TRUE);
    }

    bool terrain_height_at(float world_x, float world_z, float reference_y, float& out_height) const {
        float best_height = reference_y;
        float best_distance = std::numeric_limits<float>::max();
        for (const auto& instance : instances) {
            const float local_x = world_x - instance.origin_x;
            const float local_z = world_z - instance.origin_z;
            if (local_x < -0.01f || local_x > config.tile_size + 0.01f ||
                local_z < -0.01f || local_z > config.tile_size + 0.01f) {
                continue;
            }
            const auto& positions = instance.resource->positions;
            const auto& indices = instance.resource->indices;
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                const auto& a = positions[indices[i]];
                const auto& b = positions[indices[i + 1]];
                const auto& c = positions[indices[i + 2]];
                const float denominator =
                    (b.z - c.z) * (a.x - c.x) +
                    (c.x - b.x) * (a.z - c.z);
                if (std::abs(denominator) <= 0.000001f) {
                    continue;
                }
                const float wa =
                    ((b.z - c.z) * (local_x - c.x) +
                     (c.x - b.x) * (local_z - c.z)) / denominator;
                const float wb =
                    ((c.z - a.z) * (local_x - c.x) +
                     (a.x - c.x) * (local_z - c.z)) / denominator;
                const float wc = 1.0f - wa - wb;
                if (wa < -0.001f || wb < -0.001f || wc < -0.001f) {
                    continue;
                }
                const float height = wa * a.y + wb * b.y + wc * c.y;
                const float distance = std::abs(height - reference_y);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_height = height;
                }
            }
        }
        if (best_distance < std::numeric_limits<float>::max()) {
            out_height = best_height;
            return true;
        }
        return false;
    }

    bool terrain_surface_at(float world_x, float world_z, float& out_height, Vec3& out_normal) const {
        float best_height = -std::numeric_limits<float>::max();
        Vec3 best_normal{};
        bool found = false;
        for (const auto& instance : instances) {
            const float local_x = world_x - instance.origin_x;
            const float local_z = world_z - instance.origin_z;
            if (local_x < -0.01f || local_x > config.tile_size + 0.01f ||
                local_z < -0.01f || local_z > config.tile_size + 0.01f) {
                continue;
            }
            const auto& positions = instance.resource->positions;
            const auto& indices = instance.resource->indices;
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                const auto& a = positions[indices[i]];
                const auto& b = positions[indices[i + 1]];
                const auto& c = positions[indices[i + 2]];
                const float denominator =
                    (b.z - c.z) * (a.x - c.x) +
                    (c.x - b.x) * (a.z - c.z);
                if (std::abs(denominator) <= 0.000001f) {
                    continue;
                }
                const float wa =
                    ((b.z - c.z) * (local_x - c.x) +
                     (c.x - b.x) * (local_z - c.z)) / denominator;
                const float wb =
                    ((c.z - a.z) * (local_x - c.x) +
                     (a.x - c.x) * (local_z - c.z)) / denominator;
                const float wc = 1.0f - wa - wb;
                if (wa < -0.001f || wb < -0.001f || wc < -0.001f) {
                    continue;
                }
                const float height = wa * a.y + wb * b.y + wc * c.y;
                if (!found || height > best_height) {
                    best_height = height;
                    best_normal = normalize(cross(subtract(b, a), subtract(c, a)));
                    if (best_normal.y < 0.0f) {
                        best_normal = scale(best_normal, -1.0f);
                    }
                    found = true;
                }
            }
        }
        if (!found) {
            return false;
        }
        out_height = best_height;
        out_normal = best_normal;
        return true;
    }

    bool flat_grass_surface_at(float world_x, float world_z, float& out_height, Vec3& out_normal) const {
        float center_height = 0.0f;
        Vec3 center_normal{};
        if (!terrain_surface_at(world_x, world_z, center_height, center_normal) ||
            std::abs(center_normal.y) < config.grass_flatness_normal_y) {
            return false;
        }
        const float radius = config.grass_flatness_radius;
        const float diagonal = radius * 0.70710678f;
        const Vec3 offsets[] = {
            {radius, 0.0f, 0.0f},
            {-radius, 0.0f, 0.0f},
            {0.0f, 0.0f, radius},
            {0.0f, 0.0f, -radius},
            {diagonal, 0.0f, diagonal},
            {-diagonal, 0.0f, diagonal},
            {diagonal, 0.0f, -diagonal},
            {-diagonal, 0.0f, -diagonal},
        };
        for (const auto& offset : offsets) {
            float height = 0.0f;
            Vec3 normal{};
            if (!terrain_surface_at(world_x + offset.x, world_z + offset.z, height, normal)) {
                return false;
            }
            const float expected_height =
                center_height -
                (center_normal.x * offset.x + center_normal.z * offset.z) / center_normal.y;
            if (std::abs(height - expected_height) > config.grass_flatness_threshold) {
                return false;
            }
        }
        out_height = center_height;
        out_normal = center_normal;
        return true;
    }

    void snap_to_ground() {
        float height = 0.0f;
        if (terrain_height_at(spawn_x, spawn_z, spawn_y, height)) {
            spawn_y = height;
        }
    }

    bool collides_with_static(float x, float y, float z) const {
        const float radius = config.player_collision_radius;
        const float radius_squared = radius * radius;
        const float body_top = y - config.player_collision_height;
        for (const auto& instance : static_instances) {
            if (x < instance.bounds_min.x - radius || x > instance.bounds_max.x + radius ||
                z < instance.bounds_min.z - radius || z > instance.bounds_max.z + radius ||
                y < instance.bounds_min.y - radius || body_top > instance.bounds_max.y + radius) {
                continue;
            }
            const auto& positions = instance.resource->collision_positions;
            const auto& indices = instance.resource->collision_indices;
            for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3) {
                const Vec3 a = transform_point(positions[indices[triangle]], instance.world);
                const Vec3 b = transform_point(positions[indices[triangle + 1]], instance.world);
                const Vec3 c = transform_point(positions[indices[triangle + 2]], instance.world);
                const Vec3 normal = cross(subtract(b, a), subtract(c, a));
                const float normal_length = std::sqrt(dot(normal, normal));
                if (normal_length <= 0.00001f) {
                    continue;
                }
                // Walkable (floor-facing) triangles — floors, slopes, ramps — are
                // never collision walls: you stand/walk on them (support_height_at
                // handles the height). Only steep faces (walls) block horizontal
                // movement. Skipping only floors *below* the feet (the old check)
                // made the slope above your feet block you, so ramps could only be
                // jumped onto, not walked up.
                const bool floor_facing =
                    std::abs(normal.y) / normal_length >= config.collision_floor_normal_threshold;
                if (floor_facing) {
                    continue;
                }
                for (float offset = radius; offset < config.player_collision_height; offset += radius) {
                    const Vec3 center{x, y - offset, z};
                    if (point_triangle_distance_squared(center, a, b, c) <= radius_squared) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // True if (px,pz) is inside triangle (a,b,c) projected onto the XZ plane.
    static bool point_in_triangle_xz(float px, float pz, const Vec3& a, const Vec3& b, const Vec3& c) {
        const float d1 = (px - b.x) * (a.z - b.z) - (a.x - b.x) * (pz - b.z);
        const float d2 = (px - c.x) * (b.z - c.z) - (b.x - c.x) * (pz - c.z);
        const float d3 = (px - a.x) * (c.z - a.z) - (c.x - a.x) * (pz - a.z);
        const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(neg && pos);
    }

    // Highest (smallest y, since world +y is down) solid floor-facing static
    // surface directly above/below (x,z) within the y window [min_y, max_y].
    // Lets the player stand on and walk up static objects (ramps, diagonal beams)
    // the way the original's vertical collision rests the body on the world mesh.
    bool static_floor_height_at(float x, float z, float min_y, float max_y, float& out_y,
                                Vec3* out_normal = nullptr) const {
        bool found = false;
        float best = max_y;
        Vec3 best_normal{0.0f, -1.0f, 0.0f};
        for (const auto& instance : static_instances) {
            if (x < instance.bounds_min.x || x > instance.bounds_max.x ||
                z < instance.bounds_min.z || z > instance.bounds_max.z ||
                instance.bounds_min.y > max_y || instance.bounds_max.y < min_y) {
                continue;
            }
            const auto& positions = instance.resource->collision_positions;
            const auto& indices = instance.resource->collision_indices;
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                const Vec3 a = transform_point(positions[indices[t]], instance.world);
                const Vec3 b = transform_point(positions[indices[t + 1]], instance.world);
                const Vec3 c = transform_point(positions[indices[t + 2]], instance.world);
                const Vec3 normal = cross(subtract(b, a), subtract(c, a));
                const float len = std::sqrt(dot(normal, normal));
                if (len <= 0.00001f || std::abs(normal.y) / len < config.collision_floor_normal_threshold) {
                    continue;  // only walkable (floor-facing) triangles
                }
                if (!point_in_triangle_xz(x, z, a, b, c)) {
                    continue;
                }
                // Plane height at (x,z): y = a.y - (nx*(x-a.x)+nz*(z-a.z))/ny.
                const float y = a.y - (normal.x * (x - a.x) + normal.z * (z - a.z)) / normal.y;
                if (y < min_y || y > max_y) {
                    continue;
                }
                if (!found || y < best) {
                    best = y;
                    found = true;
                    const float inv = 1.0f / len;
                    best_normal = Vec3{normal.x * inv, normal.y * inv, normal.z * inv};
                }
            }
        }
        if (found) {
            out_y = best;
            if (out_normal) {
                *out_normal = best_normal;
            }
        }
        return found;
    }

    // The support height at (x,z) for a body whose feet are at feet_y: the highest
    // of the terrain and any static surface reachable by a step (up or down). +y
    // is down, so "higher" = smaller y.
    bool support_height_at(float x, float z, float feet_y, float& out_y, Vec3* out_normal = nullptr) const {
        // Window centred on the FEET (step up or down). A surface only supports
        // the body if it lies within this window — otherwise there is nothing to
        // stand on here (the body should fall, not snap down to far-below terrain,
        // which looked like falling through the model when standing on an object).
        const float reach_up = feet_y - config.max_step_height;
        const float reach_down = feet_y + config.max_step_height;
        bool found = false;
        float floor = 0.0f;
        Vec3 normal{0.0f, -1.0f, 0.0f};
        float terrain = 0.0f;
        Vec3 terrain_normal{};
        if (terrain_surface_at(x, z, terrain, terrain_normal) && terrain >= reach_up && terrain <= reach_down) {
            floor = terrain;
            normal = terrain_normal;
            found = true;
        }
        float obj = 0.0f;
        Vec3 obj_normal{};
        if (static_floor_height_at(x, z, reach_up, reach_down, obj, &obj_normal) && (!found || obj < floor)) {
            floor = obj;  // stand on the object surface (higher than terrain)
            normal = obj_normal;
            found = true;
        }
        if (found) {
            out_y = floor;
            if (out_normal) {
                *out_normal = normal;
            }
        }
        return found;
    }

    bool try_move_to(float x, float z) {
        if (grounded) {
            // On the ground the body rests on the support surface (step up/down
            // onto terrain or objects within max_step_height).
            float ground_y = 0.0f;
            if (!support_height_at(x, z, spawn_y, ground_y)) {
                return false;
            }
            if (std::abs(ground_y - spawn_y) > config.max_step_height ||
                collides_with_static(x, ground_y, z)) {
                return false;
            }
            spawn_x = x;
            spawn_y = ground_y;
            spawn_z = z;
        } else {
            // Airborne: keep the jump-controlled height, just move horizontally.
            // Do NOT require a support surface here — once you jump higher than a
            // step the ground falls outside the support window, and requiring it
            // would freeze all horizontal motion mid-air ("jump in place").
            if (collides_with_static(x, spawn_y, z)) {
                return false;
            }
            spawn_x = x;
            spawn_z = z;
        }
        return true;
    }

    void jump() {
        if (grounded) {
            velocity_y = config.jump_impulse;
            grounded = false;
        }
    }

    // Gravity drags the body down a steep floor while grounded (slide down a ramp
    // when stopped). Uses the real floor normal: tangential gravity = G - (G·n)n
    // with G = (0, gravity, 0) (+y is down); its horizontal part is the downhill
    // direction, its magnitude g·sin(slope). Only floors steeper than the friction
    // limit (config.slope_slide_normal_y) slide.
    void apply_slope_slide(float delta_seconds) {
        if (!grounded || delta_seconds <= 0.0f) {
            return;
        }
        float floor_y = 0.0f;
        Vec3 n{};
        if (!support_height_at(spawn_x, spawn_z, spawn_y, floor_y, &n)) {
            return;
        }
        const float ny = std::abs(n.y);
        if (ny >= config.slope_slide_normal_y || ny <= 0.0001f) {
            return;  // gentle enough to stand on (or degenerate)
        }
        const float g = config.jump_gravity;
        const float tx = -g * n.y * n.x;
        const float tz = -g * n.y * n.z;
        const float hlen = std::sqrt(tx * tx + tz * tz);
        if (hlen < 1e-4f) {
            return;
        }
        const float speed = g * std::sqrt((std::max)(0.0f, 1.0f - ny * ny)) * config.slope_slide_factor;
        const float disp = speed * delta_seconds;
        try_move_to(spawn_x + (tx / hlen) * disp, spawn_z + (tz / hlen) * disp);
    }

    void update_vertical(float delta_seconds) {
        if (grounded || delta_seconds <= 0.0f) {
            return;
        }
        velocity_y += config.jump_gravity * delta_seconds;
        const float prev_y = spawn_y;
        spawn_y += velocity_y * delta_seconds;
        if (velocity_y < 0.0f) {
            return;  // still rising (jump apex not reached); can't land
        }
        // Land on the highest surface (terrain or a static object) crossed during
        // this descent, so you can land on top of objects, not only the terrain.
        float floor = 0.0f;
        bool found = terrain_height_at(spawn_x, spawn_z, spawn_y, floor);
        float obj = 0.0f;
        if (static_floor_height_at(spawn_x, spawn_z, prev_y - 0.05f, spawn_y + 0.05f, obj) &&
            (!found || obj < floor)) {
            floor = obj;
            found = true;
        }
        if (found && spawn_y >= floor) {
            spawn_y = floor;
            velocity_y = 0.0f;
            grounded = true;
        }
    }

    void update_view_projection() {
        const RECT rc = client_rect();
        const float aspect = static_cast<float>(rc.right - rc.left) / static_cast<float>(rc.bottom - rc.top);
        // Sphere's renderer uses positive Y downward; the server/Godot side
        // stores the same position with the Y sign reversed.
        // The first-person camera rides at the player's head bone (so the body
        // is visible below and the near clip culls the player's own head),
        // falling back to a fixed eye height before the model is skinned.
        Vec3 eye{spawn_x, spawn_y - config.camera_eye_height, spawn_z};
        if (player_eye_valid) {
            // Eye offset rotates with the body (camera_yaw), keeping the lens at
            // the face front regardless of facing.
            const float c = std::cos(camera_yaw);
            const float s = std::sin(camera_yaw);
            eye.x = spawn_x + player_eye_local_x * c + player_eye_local_z * s;
            eye.y = spawn_y + player_eye_local_y;
            eye.z = spawn_z - player_eye_local_x * s + player_eye_local_z * c;
            // While walking, let the eye follow the head-top's vertical motion
            // through the stride for a small up/down bob (idle stays locked).
            if (player_walking) {
                eye.y += (player_live_crown_y - player_locked_crown_y) * kWalkBobScale;
            }
        }
        const float horizontal_distance = std::cos(camera_pitch) * config.camera_look_distance;
        const Vec3 target{
            eye.x + std::sin(camera_yaw) * horizontal_distance,
            eye.y - std::sin(camera_pitch) * config.camera_look_distance,
            eye.z + std::cos(camera_yaw) * horizontal_distance,
        };
        camera_eye = eye;
        camera_target = target;
        const auto view = look_at_rh_matrix(eye, target, Vec3{0.0f, -1.0f, 0.0f});
        const auto projection = perspective_fov_rh_matrix(config.camera_fov * kPi / 180.0f, aspect, config.near_clip, config.far_clip);
        view_matrix = view;
        projection_matrix = projection;
        view_projection_matrix = multiply_matrix(view, projection);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
    }

    void draw_terrain() {
        // Terrain stays fixed-function: it needs the tile texture (uv0) modulated
        // by the microtexture detail layer on a separate finer UV (uv1). The base
        // shader samples only one texture/UV, so routing terrain through it lost
        // the detail and made the ground a smeared single-tile texture. A faithful
        // 2-UV terrain shader is a later step.
        device->SetFVF(kWorldVertexFvf);
        device->SetTexture(1, terrain_microtexture);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        device->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        for (const auto& instance : instances) {
            const auto* resource = instance.resource;
            const auto world = translation_matrix(instance.origin_x, 0.0f, instance.origin_z);
            device->SetTransform(D3DTS_WORLD, &world);
            device->SetStreamSource(0, resource->vertex_buffer, 0, sizeof(WorldVertex));
            device->SetIndices(resource->index_buffer);
            device->SetTexture(0, resource->texture);
            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, resource->vertex_count, 0, resource->index_count / 3);
        }
        device->SetTexture(1, nullptr);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }

    void draw_static_objects() {
        device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHAREF, 0x20);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        // Static props stay double-sided (CULL_NONE, as the original): culling
        // them broke foliage (leaf cards are single-sided geometry). Walking into
        // a wall is prevented by collision, not by culling, so you never get
        // inside a model to see its interior.
        // Lit + textured via the original base shader pair (verified).
        const bool use_shader = world_shaders_ready;
        if (use_shader) {
            begin_base_shader();
            set_base_light_constants();
        } else {
            device->SetFVF(kWorldVertexFvf);
        }
        for (const auto& instance : static_instances) {
            const auto* resource = instance.resource;
            if (use_shader) {
                set_base_world(instance.world);
            } else {
                device->SetTransform(D3DTS_WORLD, &instance.world);
            }
            device->SetStreamSource(0, resource->vertex_buffer, 0, sizeof(WorldVertex));
            device->SetIndices(resource->index_buffer);
            for (const auto& batch : resource->batches) {
                device->SetTexture(0, batch.texture);
                device->DrawIndexedPrimitive(
                    D3DPT_TRIANGLELIST,
                    0,
                    0,
                    resource->vertex_count,
                    batch.start_index,
                    batch.index_count / 3);
            }
        }
        if (use_shader) {
            end_base_shader();
        }
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    void draw_grass() {
        device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHAREF, 0x20);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        // Lit + textured via the base shader (consistent with terrain/objects).
        // The wind is still the CPU coherent-wave approximation applied to each
        // instance's world matrix below; the original's per-vertex shader wind
        // (VS 04_0x with gWindCircle/gMiniSinTable) needs grass geometry carrying
        // per-vertex wind attributes (pivot/bend/phase/length), a separate task.
        const bool use_shader = world_shaders_ready;
        if (use_shader) {
            begin_base_shader();
            set_base_light_constants();
        } else {
            device->SetFVF(kWorldVertexFvf);
        }
        for (const auto& instance : grass_instances) {
            const auto* resource = instance.resource;
            auto world = instance.world;
            const float x = world._41;
            const float y = world._42;
            const float z = world._43;
            world._41 = 0.0f;
            world._42 = 0.0f;
            world._43 = 0.0f;
            if (config.grass_quality == 2) {
                // Coherent wind: a wave travels across the field along a fixed
                // wind direction so neighbouring grass bends together in gusts
                // (approximating the original's vertex-shader wind) instead of
                // every blade shimmering at an independent random phase.
                constexpr float kWindDirX = 0.70f;
                constexpr float kWindDirZ = 0.71f;
                constexpr float kSpatialFreq = 0.06f;  // radians per world unit
                const float spatial = (x * kWindDirX + z * kWindDirZ) * kSpatialFreq;
                const float t = elapsed_seconds * config.grass_wind_speed;
                // Slow gust envelope so the wind swells and calms over the field.
                const float gust = 0.55f + 0.45f * std::sin(t * 0.21f + spatial * 0.5f);
                const float sway =
                    std::sin(t - spatial + instance.wind_phase * 0.12f) *
                    config.grass_wind_amplitude * instance.wind_scale * gust;
                world = multiply_matrix(world, rotation_z_matrix(sway));
            }
            world._41 = x;
            world._42 = y;
            world._43 = z;
            if (use_shader) {
                set_base_world(world);
            } else {
                device->SetTransform(D3DTS_WORLD, &world);
            }
            device->SetStreamSource(0, resource->vertex_buffer, 0, sizeof(WorldVertex));
            device->SetIndices(resource->index_buffer);
            for (const auto& batch : resource->batches) {
                device->SetTexture(0, batch.texture);
                device->DrawIndexedPrimitive(
                    D3DPT_TRIANGLELIST,
                    0,
                    0,
                    resource->vertex_count,
                    batch.start_index,
                    batch.index_count / 3);
            }
        }
        if (use_shader) {
            end_base_shader();
        }
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    void draw_sky() {
        constexpr int rings = 8;
        constexpr int segments = 32;
        std::vector<WorldVertex> vertices;
        std::vector<std::uint16_t> indices;
        vertices.reserve((rings + 1) * (segments + 1));
        indices.reserve(rings * segments * 6);
        const DWORD color = D3DCOLOR_XRGB(environment_cloud_red, environment_cloud_green, environment_cloud_blue);
        const float scroll = elapsed_seconds * config.sky_scroll_speed;
        for (int ring = 0; ring <= rings; ++ring) {
            const float v = static_cast<float>(ring) / static_cast<float>(rings);
            const float theta = v * kPi * 0.5f;
            const float radial = std::sin(theta) * config.sky_radius;
            const float height = std::cos(theta) * config.sky_radius * config.sky_height_scale;
            for (int segment = 0; segment <= segments; ++segment) {
                const float u = static_cast<float>(segment) / static_cast<float>(segments);
                const float angle = u * 2.0f * kPi;
                vertices.push_back(WorldVertex{
                    spawn_x + std::cos(angle) * radial,
                    spawn_y - height,
                    spawn_z + std::sin(angle) * radial,
                    0.0f,
                    -1.0f,
                    0.0f,
                    color,
                    u + scroll,
                    v,
                    0.0f,
                    0.0f,
                });
            }
        }
        for (int ring = 0; ring < rings; ++ring) {
            for (int segment = 0; segment < segments; ++segment) {
                const auto a = static_cast<std::uint16_t>(ring * (segments + 1) + segment);
                const auto b = static_cast<std::uint16_t>(a + segments + 1);
                indices.push_back(a);
                indices.push_back(b);
                indices.push_back(static_cast<std::uint16_t>(a + 1));
                indices.push_back(static_cast<std::uint16_t>(a + 1));
                indices.push_back(b);
                indices.push_back(static_cast<std::uint16_t>(b + 1));
            }
        }

        // clouds.dds is a DARK cloud texture (clouds = bright, clear sky = near
        // black) meant to be ADDED over the sky-coloured background, not drawn
        // opaque. The frame is already cleared to the sky colour (environment
        // clear), so blend the tinted cloud texture additively: bright cloud
        // texels brighten the blue sky into clouds, dark texels leave it blue.
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetFVF(kWorldVertexFvf);
        device->SetTexture(0, sky_texture);
        const auto identity = identity_matrix();
        device->SetTransform(D3DTS_WORLD, &identity);
        device->DrawIndexedPrimitiveUP(
            D3DPT_TRIANGLELIST,
            0,
            static_cast<UINT>(vertices.size()),
            static_cast<UINT>(indices.size() / 3),
            indices.data(),
            D3DFMT_INDEX16,
            vertices.data(),
            sizeof(WorldVertex));
        configure_render_state();
    }

    // Representative water-surface Y for the planar-reflection plane: the height
    // of the water-bearing patch nearest the camera (most water in a region sits
    // at one level). Returns false when no water is visible.
    bool water_plane(float& out_y) const {
        bool found = false;
        float best_d2 = 0.0f;
        const float half = config.tile_size * 0.5f;
        for (const auto& instance : instances) {
            if (!instance.resource->has_water) {
                continue;
            }
            const float cx = instance.origin_x + half;
            const float cz = instance.origin_z + half;
            const float dx = cx - spawn_x;
            const float dz = cz - spawn_z;
            const float d2 = dx * dx + dz * dz;
            if (!found || d2 < best_d2) {
                best_d2 = d2;
                out_y = instance.resource->water_height;
                found = true;
            }
        }
        return found;
    }

    // Reflection strength for the current time of day (decoded FUN_004db5e0): the
    // water lerps base colour↔reflection by this. 0 in full day (pure water colour),
    // rises through dawn/dusk to the transition peak, moderate at deep night.
    float water_reflect_coeff() const {
        const float t = game_time_fraction;
        float grad = 0.0f;
        float mult = 0.0f;
        if (t >= config.water_day_start && t <= config.water_day_end) {
            grad = 0.0f;  // full day → no reflection
            mult = 0.0f;
        } else if (t <= config.water_night_before || t >= config.water_night_after) {
            grad = 1.0f;  // deep night
            mult = config.water_reflect_night;
        } else if (t < config.water_day_start) {
            grad = 1.0f - (t - config.water_night_before) / config.water_transition_width;  // dawn ramp
            mult = config.water_reflect_transition;
        } else {
            grad = (t - config.water_day_end) / config.water_transition_width;  // dusk ramp
            mult = config.water_reflect_transition;
        }
        return std::clamp(grad, 0.0f, 1.0f) * mult;
    }

    // Planar reflection (the original's reflection RT, FUN_004617a0/FUN_0047cc60):
    // re-render sky + terrain + objects mirrored about the water plane into the
    // reflection texture, which draw_water then projects onto the surface.
    void render_reflection() {
        if (config.water_reflection_enabled == 0 || !reflection_surface || !reflection_depth) {
            return;
        }
        float water_y = 0.0f;
        if (!water_plane(water_y)) {
            return;
        }
        IDirect3DSurface9* prev_rt = nullptr;
        IDirect3DSurface9* prev_depth = nullptr;
        device->GetRenderTarget(0, &prev_rt);
        device->GetDepthStencilSurface(&prev_depth);
        device->SetRenderTarget(0, reflection_surface);
        device->SetDepthStencilSurface(reflection_depth);
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                      D3DCOLOR_XRGB(environment_clear_red, environment_clear_green, environment_clear_blue),
                      1.0f, 0);

        // Mirror the camera across y = water_y (world +Y is down, so the up vector
        // flips from -Y to +Y).
        const Vec3 eye{camera_eye.x, 2.0f * water_y - camera_eye.y, camera_eye.z};
        const Vec3 target{camera_target.x, 2.0f * water_y - camera_target.y, camera_target.z};
        const D3DMATRIX saved_view = view_matrix;
        const D3DMATRIX saved_vp = view_projection_matrix;
        const auto view = look_at_rh_matrix(eye, target, Vec3{0.0f, 1.0f, 0.0f});
        view_matrix = view;
        view_projection_matrix = multiply_matrix(view, projection_matrix);
        device->SetTransform(D3DTS_VIEW, &view);
        // Reflection reverses triangle winding; force two-sided so nothing vanishes.
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

        rendering_reflection = true;
        draw_sky();
        draw_terrain();
        draw_static_objects();
        rendering_reflection = false;

        // Restore the main camera + back buffer.
        view_matrix = saved_view;
        view_projection_matrix = saved_vp;
        device->SetTransform(D3DTS_VIEW, &saved_view);
        device->SetRenderTarget(0, prev_rt);
        device->SetDepthStencilSurface(prev_depth);
        release_com(prev_rt);
        release_com(prev_depth);
        configure_render_state();
    }

    // Per-frame wave displacement (FUN_0046a070): rewrite each water vertex's Y =
    // (sin(phase)+amp)*scale + baseY, phase from world XZ (continuous across patches)
    // + time. Locks the patch water VB and uploads the displaced positions.
    void update_water_waves(TerrainResource* resource, float origin_x, float origin_z) {
        if (!resource->water_vertex_buffer || resource->water_cpu_verts.empty() ||
            config.wave_scale <= 0.0f) {
            return;
        }
        const float kx = config.wave_freq_x / config.wave_cell_step;
        const float kz = config.wave_freq_z / config.wave_cell_step;
        const float time_phase = elapsed_seconds * config.wave_speed;
        void* p = nullptr;
        if (FAILED(resource->water_vertex_buffer->Lock(0, 0, &p, 0))) {
            return;
        }
        auto* out = static_cast<WorldVertex*>(p);
        for (std::size_t i = 0; i < resource->water_cpu_verts.size(); ++i) {
            WorldVertex v = resource->water_cpu_verts[i];
            const float phase = (origin_x + v.x) * kx + (origin_z + v.z) * kz + time_phase;
            v.y = resource->water_height + (std::sin(phase) + config.wave_amp) * config.wave_scale;
            out[i] = v;
        }
        resource->water_vertex_buffer->Unlock();
    }

    // Increment 1: translucent textured water surfaces from the .wtr grids,
    // fixed-function (alpha-blended river texture, no depth write). The reflective
    // shader (PS 01_00 + VS 03_00, env-cube + wave normals) is a later increment.
    void draw_water() {
        bool any = false;
        for (const auto& instance : instances) {
            if (instance.resource->water_index_count > 0) {
                any = true;
                break;
            }
        }
        if (!any || !water_texture) {
            return;
        }
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kWorldVertexFvf);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);   // blend over the scene, don't occlude
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(0xB0, 0xFF, 0xFF, 0xFF));
        // Project the reflection only when the eye is ABOVE the water surface (world
        // +Y is down → above means a smaller Y than the plane). From below/inside the
        // water a sky reflection is meaningless and, on the plane's back face, appears
        // to swing as the camera turns — so fall back to the flat texture there.
        // Reflection is gated only by the REFLQUAL graphics option (native:
        // FUN_0046a070 does the reflection pass when 0 < DAT_04f49a9c) + a valid RT.
        const bool use_reflection =
            config.water_reflection_enabled != 0 && reflection_texture != nullptr;
        if (use_reflection) {
            // Project the planar-reflection RT onto the surface: texcoord = the
            // surface point's own screen position (the RT holds the mirrored scene
            // at those same screen pixels). texMtx maps view-space pos → clip → [0,1].
            const D3DMATRIX bias = {
                0.5f,  0.0f,  0.0f, 0.0f,
                0.0f, -0.5f,  0.0f, 0.0f,
                0.0f,  0.0f,  1.0f, 0.0f,
                0.5f,  0.5f,  0.0f, 1.0f,
            };
            const D3DMATRIX tex_mtx = multiply_matrix(projection_matrix, bias);
            device->SetTransform(D3DTS_TEXTURE0, &tex_mtx);
            device->SetTexture(0, reflection_texture);
            device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
            device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            // Blend reflection ↔ the water's own colour by the time-of-day reflect
            // coefficient (decoded FUN_004db5e0): result = k*reflection + (1-k)*base.
            // Base colour = the environment sky/clear colour (data-driven, Sky.txt) —
            // a stand-in for the native Fresnel gradient until step 4 (the shader).
            const int k = std::clamp(static_cast<int>(water_reflect_coeff() * 255.0f + 0.5f), 0, 255);
            device->SetTextureStageState(0, D3DTSS_CONSTANT,
                D3DCOLOR_ARGB(k, environment_clear_red, environment_clear_green, environment_clear_blue));
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_LERP);
            device->SetTextureStageState(0, D3DTSS_COLORARG0, D3DTA_CONSTANT | D3DTA_ALPHAREPLICATE);  // k
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);                          // reflection
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CONSTANT);                         // base water colour
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);  // constant translucency
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        } else {
            device->SetTexture(0, water_texture);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);  // constant translucency
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        }
        for (const auto& instance : instances) {
            auto* resource = instance.resource;
            if (resource->water_index_count == 0) {
                continue;
            }
            update_water_waves(resource, instance.origin_x, instance.origin_z);
            const auto world = translation_matrix(instance.origin_x, 0.0f, instance.origin_z);
            device->SetTransform(D3DTS_WORLD, &world);
            device->SetStreamSource(0, resource->water_vertex_buffer, 0, sizeof(WorldVertex));
            device->SetIndices(resource->water_index_buffer);
            device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST, 0, 0, resource->water_vertex_count, 0, resource->water_index_count / 3);
        }
        if (use_reflection) {
            // Undo the projective texture transform so later passes are unaffected.
            device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        }
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        configure_render_state();  // restore the shared (shader-era) render state
    }

    void draw_player() {
        if (!player_vertex_buffer || !player_index_buffer || player_batches.empty()) {
            return;
        }
        // The body faces where the camera looks, so in first person it stays
        // locked below the view as you turn (camera rides inside the head).
        auto world = rotation_y_matrix(camera_yaw);
        // Ease the body backward along the look axis while moving so the
        // forward-leaning torso's neck cut never enters the lens.
        world._41 = spawn_x - std::sin(camera_yaw) * player_body_shift;
        world._42 = spawn_y;
        world._43 = spawn_z - std::cos(camera_yaw) * player_body_shift;
        const bool use_shader = world_shaders_ready;
        if (use_shader) {
            // The body is already skinned to world-posed vertices on the CPU, so
            // the base lit+textured shader applies like any static object.
            begin_base_shader();
            set_base_light_constants();
            set_base_world(world);
        } else {
            device->SetTransform(D3DTS_WORLD, &world);
            device->SetFVF(kWorldVertexFvf);
        }
        device->SetStreamSource(0, player_vertex_buffer, 0, sizeof(WorldVertex));
        device->SetIndices(player_index_buffer);
        // Backface-cull the body so looking down does not reveal the dark
        // interior of the torso/neck. The rest of the world draws double-sided.
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        for (const auto& batch : player_batches) {
            device->SetTexture(0, batch.texture);
            device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,
                0,
                0,
                player_vertex_count,
                batch.start_index,
                batch.index_count / 3);
        }
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (use_shader) {
            end_base_shader();
        }
    }

    void draw_overlay() {
        if (!overlay_texture || overlay_width <= 0 || overlay_height <= 0) {
            return;
        }
        const float w = static_cast<float>(overlay_width);
        const float h = static_cast<float>(overlay_height);
        const OverlayVertex quad[] = {
            {-0.5f, -0.5f, 0.0f, 1.0f, 0xffffffff, 0.0f, 0.0f},
            {w - 0.5f, -0.5f, 0.0f, 1.0f, 0xffffffff, 1.0f, 0.0f},
            {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 0xffffffff, 1.0f, 1.0f},
            {-0.5f, h - 0.5f, 0.0f, 1.0f, 0xffffffff, 0.0f, 1.0f},
        };
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetFVF(kOverlayVertexFvf);
        device->SetTexture(0, overlay_texture);
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(OverlayVertex));
        configure_render_state();
    }

    bool initialize(
        HWND window,
        const std::filesystem::path& root,
        const LuaGameWindowConfig& world_config,
        const SkinnedCharacterModel* player_model_in,
        double x,
        double y,
        double z,
        double angle,
        std::wstring& error) {
        hwnd = window;
        root_path = root;
        config = world_config;
        environment_clear_red = config.clear_red;
        environment_clear_green = config.clear_green;
        environment_clear_blue = config.clear_blue;
        environment_cloud_red = config.sky_red;
        environment_cloud_green = config.sky_green;
        environment_cloud_blue = config.sky_blue;
        spawn_x = static_cast<float>(x);
        spawn_y = static_cast<float>(y);
        spawn_z = static_cast<float>(z);
        spawn_angle = static_cast<float>(angle);
        camera_yaw = -spawn_angle;
        if (!create_device(error)) {
            return false;
        }
        create_reflection_target();
        load_world_shaders();
        try {
            terrain_microtexture = load_mtx_texture(device, root_path / config.terrain_microtexture);
            sky_texture = load_dds_texture(device, root_path / config.sky_texture);
            {  // water surface texture (animated river frame; static for now)
                const auto wpath = root_path / "landscape" / "river1a_00.dds";
                if (std::filesystem::exists(wpath)) {
                    water_texture = load_dds_texture(device, wpath);
                }
            }
            load_visible_terrain();
            snap_to_ground();
            load_visible_grass();
            load_static_placements();
            load_visible_static_objects();
            if (player_model_in) {
                load_player_model(*player_model_in);
            }
        } catch (const std::exception& ex) {
            assign_error(error, std::string("game world load failed: ") + ex.what());
            return false;
        }
        configure_render_state();
        initialized = true;
        return true;
    }

    bool set_overlay_bitmap(int width, int height, std::vector<std::uint8_t> pixels, std::wstring& error) {
        if (!device) {
            error = L"set_overlay_bitmap called before Direct3D device creation";
            return false;
        }
        if (width <= 0 || height <= 0 || pixels.size() < static_cast<std::size_t>(width) * height * 4) {
            error = L"invalid game overlay bitmap";
            return false;
        }
        HRESULT hr = S_OK;
        if (!overlay_texture || overlay_width != width || overlay_height != height) {
            release_com(overlay_texture);
            hr = device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &overlay_texture, nullptr);
            if (FAILED(hr)) {
                error = hresult_text("CreateTexture game overlay", hr);
                return false;
            }
        }
        D3DLOCKED_RECT locked{};
        hr = overlay_texture->LockRect(0, &locked, nullptr, 0);
        if (FAILED(hr)) {
            error = hresult_text("GameOverlayTexture::LockRect", hr);
            return false;
        }
        const std::size_t source_pitch = static_cast<std::size_t>(width) * 4;
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch,
                pixels.data() + static_cast<std::size_t>(row) * source_pitch,
                source_pitch);
        }
        overlay_texture->UnlockRect(0);
        overlay_width = width;
        overlay_height = height;
        return true;
    }

    void set_fog(float start, float end) {
        config.fog_start = start;
        config.fog_end = end;
        if (device) {
            configure_render_state();
        }
    }

    bool set_grass_quality(int quality, std::wstring& error) {
        quality = std::clamp(quality, 0, 2);
        if (config.grass_quality == quality) {
            return true;
        }
        const bool visibility_changed = (config.grass_quality == 0) != (quality == 0);
        config.grass_quality = quality;
        if (!visibility_changed) {
            return true;
        }
        grass_center_x = (std::numeric_limits<int>::min)();
        grass_center_z = (std::numeric_limits<int>::min)();
        grass_anchor_valid = false;
        try {
            load_visible_grass();
            return true;
        } catch (const std::exception& ex) {
            assign_error(error, std::string("game world grass quality update failed: ") + ex.what());
            return false;
        }
    }

    void set_game_time(float day_fraction) {
        game_time_fraction = day_fraction - std::floor(day_fraction);
        const auto& states = config.sky_states;
        if (states.size() < 2) {
            return;
        }
        std::size_t next = 0;
        while (next < states.size() && game_time_fraction >= states[next].time) {
            ++next;
        }
        const LuaSkyState* from = nullptr;
        const LuaSkyState* to = nullptr;
        float from_time = 0.0f;
        float to_time = 0.0f;
        if (next == 0) {
            from = &states.back();
            to = &states.front();
            from_time = states.back().time - 1.0f;
            to_time = states.front().time;
        } else if (next == states.size()) {
            from = &states.back();
            to = &states.front();
            from_time = states.back().time;
            to_time = states.front().time + 1.0f;
        } else {
            from = &states[next - 1];
            to = &states[next];
            from_time = from->time;
            to_time = to->time;
        }
        float sample_time = game_time_fraction;
        if (sample_time < from_time) {
            sample_time += 1.0f;
        }
        const float blend = std::clamp((sample_time - from_time) / (to_time - from_time), 0.0f, 1.0f);
        auto channel = [blend](int a, int b) {
            return static_cast<int>(std::lround(static_cast<float>(a) + static_cast<float>(b - a) * blend));
        };
        environment_clear_red = channel(from->clear_red, to->clear_red);
        environment_clear_green = channel(from->clear_green, to->clear_green);
        environment_clear_blue = channel(from->clear_blue, to->clear_blue);
        environment_ambient_red = channel(from->ambient_red, to->ambient_red);
        environment_ambient_green = channel(from->ambient_green, to->ambient_green);
        environment_ambient_blue = channel(from->ambient_blue, to->ambient_blue);
        environment_sun_red = channel(from->sun_red, to->sun_red);
        environment_sun_green = channel(from->sun_green, to->sun_green);
        environment_sun_blue = channel(from->sun_blue, to->sun_blue);
        environment_cloud_red = channel(from->cloud_red, to->cloud_red);
        environment_cloud_green = channel(from->cloud_green, to->cloud_green);
        environment_cloud_blue = channel(from->cloud_blue, to->cloud_blue);
        if (device) {
            configure_render_state();
        }
    }

    bool update(float delta_seconds, const GameMovementInput& input, std::wstring& error) {
        if (!initialized) {
            error = L"game world scene is not initialized";
            return false;
        }
        elapsed_seconds += (std::max)(0.0f, delta_seconds);
        set_game_time(game_time_fraction + (std::max)(0.0f, delta_seconds) * 12.0f / 86400.0f);
        const float forward = (input.forward ? 1.0f : 0.0f) - (input.backward ? 1.0f : 0.0f);
        const float right = (input.strafe_right ? 1.0f : 0.0f) - (input.strafe_left ? 1.0f : 0.0f);
        const float input_length = std::sqrt(forward * forward + right * right);
        const bool moving = input_length > 0.0001f && delta_seconds > 0.0f;
        update_player_animation(delta_seconds, moving, input.run);
        update_vertical(delta_seconds);

        // ControlMove keeps the own character and camera facing the same way.
        spawn_angle = -camera_yaw;

        // Velocity-based movement, matching the original ControlMove: every frame
        // the horizontal speed g_85B8 is reset to 0 and re-derived from the held
        // movement keys, then the engine integrates it (the jump only sets the
        // vertical field 0x28C, never the horizontal). So there is no momentum/glide
        // — releasing the keys stops you, even mid-air — but holding a direction
        // through a jump keeps carrying you forward (that is the "inertia").
        if (moving) {
            const float normalized_forward = forward / input_length;
            const float normalized_right = right / input_length;
            const float speed = config.walk_speed * (input.run ? config.run_multiplier : 1.0f);
            velocity_x = (std::sin(camera_yaw) * normalized_forward +
                          std::cos(camera_yaw) * normalized_right) * speed;
            velocity_z = (std::cos(camera_yaw) * normalized_forward -
                          std::sin(camera_yaw) * normalized_right) * speed;
        } else {
            velocity_x = 0.0f;
            velocity_z = 0.0f;
            if (grounded) {
                // Standing still on a steep floor: gravity drags you downhill (the
                // only time the auto-slide applies, so it never fights a climb).
                apply_slope_slide(delta_seconds);
            }
            return true;
        }

        if (delta_seconds <= 0.0f) {
            return true;
        }

        const float disp_x = velocity_x * delta_seconds;
        const float disp_z = velocity_z * delta_seconds;
        const float distance = std::sqrt(disp_x * disp_x + disp_z * disp_z);
        const int movement_steps = (std::max)(1, static_cast<int>(std::ceil(distance / config.movement_collision_step)));
        const float step_x = disp_x / static_cast<float>(movement_steps);
        const float step_z = disp_z / static_cast<float>(movement_steps);
        for (int step = 0; step < movement_steps; ++step) {
            const float previous_x = spawn_x;
            const float previous_z = spawn_z;
            if (try_move_to(previous_x + step_x, previous_z + step_z)) {
                continue;
            }
            const bool moved_x = try_move_to(previous_x + step_x, previous_z);
            const float slide_x = spawn_x;
            const float slide_z = spawn_z;
            if (!try_move_to(slide_x, slide_z + step_z) && !moved_x) {
                break;
            }
        }

        const int center_row = static_cast<int>(std::floor(spawn_x / config.tile_size)) + config.origin_row;
        const int center_column = config.origin_column - static_cast<int>(std::floor(spawn_z / config.tile_size));
        if (center_row != terrain_center_row || center_column != terrain_center_column) {
            try {
                load_visible_terrain();
                load_visible_static_objects();
            } catch (const std::exception& ex) {
                assign_error(error, std::string("game world terrain update failed: ") + ex.what());
                return false;
            }
        }
        const float grass_dx = spawn_x - grass_anchor_x;
        const float grass_dz = spawn_z - grass_anchor_z;
        if (config.grass_quality > 0 &&
            (!grass_anchor_valid ||
             grass_dx * grass_dx + grass_dz * grass_dz >=
                 config.grass_generation_margin * config.grass_generation_margin)) {
            try {
                load_visible_grass();
            } catch (const std::exception& ex) {
                assign_error(error, std::string("game world grass update failed: ") + ex.what());
                return false;
            }
        }
        return true;
    }

    void rotate_view(float mouse_dx, float mouse_dy) {
        camera_yaw += mouse_dx * config.camera_turn_speed;
        while (camera_yaw > kPi) {
            camera_yaw -= 2.0f * kPi;
        }
        while (camera_yaw < -kPi) {
            camera_yaw += 2.0f * kPi;
        }
        camera_pitch = std::clamp(
            camera_pitch - mouse_dy * config.camera_pitch_speed,
            config.camera_min_pitch,
            config.camera_max_pitch);
        spawn_angle = -camera_yaw;
    }

    GameWorldPosition position() const {
        return GameWorldPosition{spawn_x, spawn_y, spawn_z, spawn_angle};
    }

    void resize() {
        if (!device) {
            return;
        }
        fill_present_parameters();
        // Default-pool resources must be released before Reset and remade after.
        release_com(reflection_depth);
        release_com(reflection_surface);
        release_com(reflection_texture);
        if (SUCCEEDED(device->Reset(&present))) {
            create_reflection_target();
            configure_render_state();
        }
    }

    void render() {
        if (!initialized || !device) {
            return;
        }
        const HRESULT cooperative = device->TestCooperativeLevel();
        if (cooperative == D3DERR_DEVICELOST) {
            return;
        }
        if (cooperative == D3DERR_DEVICENOTRESET) {
            resize();
            return;
        }
        update_view_projection();
        if (SUCCEEDED(device->BeginScene())) {
            render_reflection();  // mirrored scene into the reflection RT (restores back buffer + main view)
            device->Clear(
                0,
                nullptr,
                D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                D3DCOLOR_XRGB(environment_clear_red, environment_clear_green, environment_clear_blue),
                1.0f,
                0);
            draw_sky();
            draw_terrain();
            draw_grass();
            draw_static_objects();
            draw_player();
            draw_water();  // translucent, after opaque geometry
            draw_overlay();
            device->EndScene();
        }
        device->Present(nullptr, nullptr, nullptr, nullptr);
    }
};

GameWorldScene::GameWorldScene() : impl_(std::make_unique<Impl>()) {}
GameWorldScene::~GameWorldScene() = default;

bool GameWorldScene::initialize(
    HWND hwnd,
    const std::filesystem::path& root,
    const LuaGameWindowConfig& config,
    const SkinnedCharacterModel* player_model,
    double spawn_x,
    double spawn_y,
    double spawn_z,
    double spawn_angle,
    std::wstring& error) {
    return impl_->initialize(hwnd, root, config, player_model, spawn_x, spawn_y, spawn_z, spawn_angle, error);
}

bool GameWorldScene::set_overlay_bitmap(int width, int height, std::vector<std::uint8_t> bgra_pixels, std::wstring& error) {
    return impl_->set_overlay_bitmap(width, height, std::move(bgra_pixels), error);
}

bool GameWorldScene::set_grass_quality(int quality, std::wstring& error) {
    return impl_->set_grass_quality(quality, error);
}

void GameWorldScene::set_fog(float start, float end) {
    impl_->set_fog(start, end);
}

void GameWorldScene::set_game_time(float day_fraction) {
    impl_->set_game_time(day_fraction);
}

float GameWorldScene::current_game_time() const {
    return impl_ ? impl_->game_time_fraction : 0.0f;
}

float GameWorldScene::camera_facing() const {
    return impl_ ? impl_->camera_yaw : 0.0f;
}

bool GameWorldScene::update(float delta_seconds, const GameMovementInput& input, std::wstring& error) {
    return impl_->update(delta_seconds, input, error);
}

void GameWorldScene::rotate_view(float mouse_dx, float mouse_dy) {
    impl_->rotate_view(mouse_dx, mouse_dy);
}

void GameWorldScene::jump() {
    impl_->jump();
}

GameWorldPosition GameWorldScene::position() const {
    return impl_->position();
}

void GameWorldScene::resize() {
    impl_->resize();
}

void GameWorldScene::render() {
    impl_->render();
}

bool GameWorldScene::valid() const {
    return impl_ && impl_->initialized;
}

} // namespace sphere::client
