#include "client/d3d_game_world_scene.hpp"

#include "common/binary_reader.hpp"
#include "model/mdl.hpp"

#include <d3d9.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
    IDirect3DVertexBuffer9* player_vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* player_index_buffer = nullptr;
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
    std::vector<PlayerBatch> player_batches;
    UINT player_vertex_count = 0;
    float spawn_x = 0.0f;
    float spawn_y = 0.0f;
    float spawn_z = 0.0f;
    float spawn_angle = 0.0f;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    int terrain_center_row = -1;
    int terrain_center_column = -1;
    int grass_center_x = (std::numeric_limits<int>::min)();
    int grass_center_z = (std::numeric_limits<int>::min)();
    float grass_anchor_x = 0.0f;
    float grass_anchor_z = 0.0f;
    float elapsed_seconds = 0.0f;
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
        }
        for (auto& [_, resource] : static_resources) {
            for (auto& batch : resource->batches) {
                release_com(batch.texture);
            }
            release_com(resource->index_buffer);
            release_com(resource->vertex_buffer);
        }
        grass_instances.clear();
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

    void load_player_mesh(const CharacterRenderMesh& mesh) {
        if (!mesh.valid()) {
            throw std::runtime_error("selected player render mesh is empty");
        }

        std::vector<WorldVertex> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const auto& source : mesh.vertices) {
            const auto normal = normalize(Vec3{source.nx, -source.ny, source.nz});
            vertices.push_back(WorldVertex{
                source.x,
                -source.y,
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

        for (const auto& source : mesh.batches) {
            if (source.index_count < 3 ||
                source.start_index > mesh.indices.size() ||
                source.index_count > mesh.indices.size() - source.start_index) {
                throw std::runtime_error("selected player render mesh contains an invalid material batch");
            }
            if (!std::filesystem::exists(source.texture_path)) {
                throw std::runtime_error("selected player texture is missing: " + source.texture_path.string());
            }
            player_batches.push_back(PlayerBatch{
                source.start_index,
                source.index_count,
                load_dds_texture(device, source.texture_path),
            });
        }

        player_vertex_count = static_cast<UINT>(vertices.size());
        const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(WorldVertex));
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
        void* vertex_data = nullptr;
        hr = player_vertex_buffer->Lock(0, vertex_bytes, &vertex_data, 0);
        if (FAILED(hr)) {
            throw std::runtime_error(hresult_text_narrow("PlayerVertexBuffer::Lock", hr));
        }
        std::memcpy(vertex_data, vertices.data(), vertex_bytes);
        player_vertex_buffer->Unlock();

        const UINT index_bytes = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint16_t));
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
        std::memcpy(index_data, mesh.indices.data(), index_bytes);
        player_index_buffer->Unlock();
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
                indices.push_back(static_cast<std::uint16_t>(first_vertex + triangle.a));
                indices.push_back(static_cast<std::uint16_t>(first_vertex + triangle.b));
                indices.push_back(static_cast<std::uint16_t>(first_vertex + triangle.c));
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
            resource->collision_indices.insert(
                resource->collision_indices.end(),
                material_indices.begin(),
                material_indices.end());
            resource->batches.push_back(batch);
        }
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
        const int map_x = static_cast<int>(std::floor(world_x)) + config.grassmap_world_offset;
        const int map_z = static_cast<int>(std::floor(world_z)) + config.grassmap_world_offset;
        const int world_resolution = config.grassmap_grid_size * config.grassmap_tile_resolution;
        if (map_x < 0 || map_z < 0 || map_x >= world_resolution || map_z >= world_resolution) {
            return 0;
        }
        const int chunk_x = map_x / config.grassmap_tile_resolution;
        const int chunk_z = map_z / config.grassmap_tile_resolution;
        const int local_x = map_x % config.grassmap_tile_resolution;
        const int local_z = map_z % config.grassmap_tile_resolution;
        const auto& map = load_grass_map(chunk_x, chunk_z);
        return map[static_cast<std::size_t>(local_z) * config.grassmap_tile_resolution + local_x] & 0x0f;
    }

    void load_visible_grass() {
        grass_instances.clear();
        if (config.grass_quality <= 0) {
            return;
        }
        if (config.grass_detail_models.empty()) {
            throw std::runtime_error("grass_detail_models is empty");
        }
        if (config.grass_sample_offsets.empty()) {
            throw std::runtime_error("grass_sample_offsets is empty");
        }
        const float spacing = config.grass_quality >= 2 ? config.grass_spacing : config.grass_spacing * 2.0f;
        const float sample_scale = spacing / config.grass_spacing;
        grass_anchor_x = spawn_x;
        grass_anchor_z = spawn_z;
        grass_anchor_valid = true;
        grass_center_x = static_cast<int>(std::floor(grass_anchor_x / spacing));
        grass_center_z = static_cast<int>(std::floor(grass_anchor_z / spacing));
        const float generation_radius = config.grass_radius + config.grass_generation_margin;
        const float radius_squared = generation_radius * generation_radius;
        const float first_x = std::floor((grass_anchor_x - generation_radius) / spacing) * spacing;
        const float first_z = std::floor((grass_anchor_z - generation_radius) / spacing) * spacing;
        for (float x = first_x; x <= grass_anchor_x + generation_radius; x += spacing) {
            for (float z = first_z; z <= grass_anchor_z + generation_radius; z += spacing) {
                const auto cell_x = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(x / spacing)));
                const auto cell_z = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(z / spacing)));
                for (std::size_t sample_index = 0; sample_index < config.grass_sample_offsets.size(); ++sample_index) {
                    const auto& sample_offset = config.grass_sample_offsets[sample_index];
                    const float sample_x = x + sample_offset.x * sample_scale;
                    const float sample_z = z + sample_offset.z * sample_scale;
                    const float dx = sample_x - grass_anchor_x;
                    const float dz = sample_z - grass_anchor_z;
                    if (dx * dx + dz * dz > radius_squared) {
                        continue;
                    }
                    const auto type = grass_type_at(sample_x, sample_z);
                    if (type == 0 || type >= config.grass_patterns.size()) {
                        continue;
                    }
                    if (config.grass_patterns[type].empty()) {
                        throw std::runtime_error("grass pattern has no models for type " + std::to_string(type));
                    }

                    std::uint32_t random_state =
                        (cell_x * 0x9e3779b9U) ^
                        (cell_z * 0x85ebca6bU) ^
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
                    if (flat_grass_surface_at(sample_x, sample_z, spawn_y, flat_height)) {
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
                        auto world = rotation_y_matrix(unit_random() * 2.0f * kPi);
                        world._41 = sample_x;
                        world._42 = flat_height - resource->second->bounds_max.y;
                        world._43 = sample_z;
                        grass_instances.push_back(GrassInstance{
                            resource->second.get(),
                            world,
                            unit_random() * 2.0f * kPi,
                            0.65f + unit_random() * 0.35f,
                        });
                        continue;
                    }

                    const int detail_count = config.grass_quality >= 2
                        ? config.grass_detail_count
                        : (std::max)(1, config.grass_detail_count / 2);
                    for (int detail = 0; detail < detail_count; ++detail) {
                        const float jitter = config.grass_spacing * config.grass_jitter_fraction;
                        const float detail_x = sample_x + (unit_random() * 2.0f - 1.0f) * jitter;
                        const float detail_z = sample_z + (unit_random() * 2.0f - 1.0f) * jitter;
                        float height = 0.0f;
                        if (!terrain_height_at(detail_x, detail_z, spawn_y, height)) {
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
                        const float yaw = unit_random() * 2.0f * kPi;
                        const float scale =
                            config.grass_scale_min + unit_random() * (config.grass_scale_max - config.grass_scale_min);
                        auto world = multiply_matrix(scale_matrix(scale), rotation_y_matrix(yaw));
                        world._41 = detail_x;
                        world._42 = height - resource->second->bounds_max.y * scale;
                        world._43 = detail_z;
                        grass_instances.push_back(GrassInstance{
                            resource->second.get(),
                            world,
                            unit_random() * 2.0f * kPi,
                            0.65f + unit_random() * 0.35f,
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
        return resource;
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

    void configure_render_state() {
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_LIGHTING, TRUE);
        device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(110, 110, 110));
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
        device->SetRenderState(D3DRS_FOGCOLOR, D3DCOLOR_XRGB(config.clear_red, config.clear_green, config.clear_blue));
        device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
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
        light.Diffuse.r = 1.0f;
        light.Diffuse.g = 0.96f;
        light.Diffuse.b = 0.88f;
        light.Ambient.r = light.Ambient.g = light.Ambient.b = 0.35f;
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

    bool flat_grass_surface_at(float world_x, float world_z, float reference_y, float& out_height) const {
        float center_height = 0.0f;
        if (!terrain_height_at(world_x, world_z, reference_y, center_height)) {
            return false;
        }
        float min_height = center_height;
        float max_height = center_height;
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
            if (!terrain_height_at(world_x + offset.x, world_z + offset.z, center_height, height)) {
                return false;
            }
            min_height = (std::min)(min_height, height);
            max_height = (std::max)(max_height, height);
        }
        if (max_height - min_height > config.grass_flatness_threshold) {
            return false;
        }
        out_height = center_height;
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
                const bool floor_facing =
                    std::abs(normal.y) / normal_length >= config.collision_floor_normal_threshold;
                const float highest_point = (std::min)({a.y, b.y, c.y});
                if (floor_facing && highest_point >= y - config.player_collision_radius) {
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

    bool try_move_to(float x, float z) {
        float ground_y = 0.0f;
        if (!terrain_height_at(x, z, spawn_y, ground_y) ||
            std::abs(ground_y - spawn_y) > config.max_step_height ||
            collides_with_static(x, ground_y, z)) {
            return false;
        }
        spawn_x = x;
        spawn_y = ground_y;
        spawn_z = z;
        return true;
    }

    void update_view_projection() {
        const RECT rc = client_rect();
        const float aspect = static_cast<float>(rc.right - rc.left) / static_cast<float>(rc.bottom - rc.top);
        // Sphere's renderer uses positive Y downward; the server/Godot side
        // stores the same position with the Y sign reversed.
        const Vec3 eye{spawn_x, spawn_y - config.camera_eye_height, spawn_z};
        const float horizontal_distance = std::cos(camera_pitch) * config.camera_look_distance;
        const Vec3 target{
            eye.x + std::sin(camera_yaw) * horizontal_distance,
            eye.y - std::sin(camera_pitch) * config.camera_look_distance,
            eye.z + std::cos(camera_yaw) * horizontal_distance,
        };
        const auto view = look_at_rh_matrix(eye, target, Vec3{0.0f, -1.0f, 0.0f});
        const auto projection = perspective_fov_rh_matrix(config.camera_fov * kPi / 180.0f, aspect, config.near_clip, config.far_clip);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
    }

    void draw_terrain() {
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
        device->SetFVF(kWorldVertexFvf);
        for (const auto& instance : static_instances) {
            const auto* resource = instance.resource;
            device->SetTransform(D3DTS_WORLD, &instance.world);
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
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    void draw_grass() {
        device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHAREF, 0x20);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        device->SetFVF(kWorldVertexFvf);
        for (const auto& instance : grass_instances) {
            const auto* resource = instance.resource;
            auto world = instance.world;
            const float x = world._41;
            const float y = world._42;
            const float z = world._43;
            world._41 = 0.0f;
            world._42 = 0.0f;
            world._43 = 0.0f;
            const float sway =
                std::sin(elapsed_seconds * config.grass_wind_speed + instance.wind_phase) *
                config.grass_wind_amplitude *
                instance.wind_scale;
            world = multiply_matrix(world, rotation_z_matrix(sway));
            world._41 = x;
            world._42 = y;
            world._43 = z;
            device->SetTransform(D3DTS_WORLD, &world);
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
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    void draw_player() {
        if (!player_vertex_buffer || !player_index_buffer || player_batches.empty()) {
            return;
        }
        auto world = rotation_y_matrix(spawn_angle);
        world._41 = spawn_x;
        world._42 = spawn_y;
        world._43 = spawn_z;
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetFVF(kWorldVertexFvf);
        device->SetStreamSource(0, player_vertex_buffer, 0, sizeof(WorldVertex));
        device->SetIndices(player_index_buffer);
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
        const CharacterRenderMesh* player_mesh,
        double x,
        double y,
        double z,
        double angle,
        std::wstring& error) {
        hwnd = window;
        root_path = root;
        config = world_config;
        spawn_x = static_cast<float>(x);
        spawn_y = static_cast<float>(y);
        spawn_z = static_cast<float>(z);
        spawn_angle = static_cast<float>(angle);
        camera_yaw = -spawn_angle;
        if (!create_device(error)) {
            return false;
        }
        try {
            terrain_microtexture = load_mtx_texture(device, root_path / config.terrain_microtexture);
            load_visible_terrain();
            snap_to_ground();
            load_visible_grass();
            load_static_placements();
            load_visible_static_objects();
            if (player_mesh) {
                load_player_mesh(*player_mesh);
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

    bool set_grass_quality(int quality, std::wstring& error) {
        quality = std::clamp(quality, 0, 2);
        if (config.grass_quality == quality) {
            return true;
        }
        config.grass_quality = quality;
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

    bool update(float delta_seconds, const GameMovementInput& input, std::wstring& error) {
        if (!initialized) {
            error = L"game world scene is not initialized";
            return false;
        }
        elapsed_seconds += (std::max)(0.0f, delta_seconds);
        const float forward = (input.forward ? 1.0f : 0.0f) - (input.backward ? 1.0f : 0.0f);
        const float right = (input.strafe_right ? 1.0f : 0.0f) - (input.strafe_left ? 1.0f : 0.0f);
        const float input_length = std::sqrt(forward * forward + right * right);
        if (input_length <= 0.0001f || delta_seconds <= 0.0f) {
            return true;
        }

        const float normalized_forward = forward / input_length;
        const float normalized_right = right / input_length;
        const float speed = config.walk_speed * (input.run ? config.run_multiplier : 1.0f);
        const float move_x =
            std::sin(camera_yaw) * normalized_forward +
            std::cos(camera_yaw) * normalized_right;
        const float move_z =
            std::cos(camera_yaw) * normalized_forward -
            std::sin(camera_yaw) * normalized_right;
        const float distance = speed * delta_seconds;
        const int movement_steps = (std::max)(1, static_cast<int>(std::ceil(distance / config.movement_collision_step)));
        const float step_x = move_x * distance / static_cast<float>(movement_steps);
        const float step_z = move_z * distance / static_cast<float>(movement_steps);
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
        // ControlMove keeps the own character and camera facing the same way.
        spawn_angle = -camera_yaw;

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
        if (SUCCEEDED(device->Reset(&present))) {
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
        device->Clear(
            0,
            nullptr,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            D3DCOLOR_XRGB(config.clear_red, config.clear_green, config.clear_blue),
            1.0f,
            0);
        if (SUCCEEDED(device->BeginScene())) {
            draw_terrain();
            draw_grass();
            draw_static_objects();
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
    const CharacterRenderMesh* player_mesh,
    double spawn_x,
    double spawn_y,
    double spawn_z,
    double spawn_angle,
    std::wstring& error) {
    return impl_->initialize(hwnd, root, config, player_mesh, spawn_x, spawn_y, spawn_z, spawn_angle, error);
}

bool GameWorldScene::set_overlay_bitmap(int width, int height, std::vector<std::uint8_t> bgra_pixels, std::wstring& error) {
    return impl_->set_overlay_bitmap(width, height, std::move(bgra_pixels), error);
}

bool GameWorldScene::set_grass_quality(int quality, std::wstring& error) {
    return impl_->set_grass_quality(quality, error);
}

bool GameWorldScene::update(float delta_seconds, const GameMovementInput& input, std::wstring& error) {
    return impl_->update(delta_seconds, input, error);
}

void GameWorldScene::rotate_view(float mouse_dx, float mouse_dy) {
    impl_->rotate_view(mouse_dx, mouse_dy);
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
