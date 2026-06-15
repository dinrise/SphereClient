#include "client/d3d_character_scene.hpp"

#include "common/binary_reader.hpp"
#include "model/chr.hpp"
#include "model/mdl.hpp"
#include "model/skl.hpp"

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sphere::client {
namespace {

constexpr DWORD kSceneVertexFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr DWORD kOverlayVertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr std::size_t kCharacterFreeAction = 20;

struct SceneVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
    DWORD diffuse = 0xffffffff;
    float u = 0.0f;
    float v = 0.0f;
};

struct OverlayVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rhw = 1.0f;
    DWORD diffuse = 0xffffffff;
    float u = 0.0f;
    float v = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ObjectPose {
    Vec3 position;
    Quat rotation;
};

struct SceneBatch {
    UINT start_index = 0;
    UINT index_count = 0;
    std::filesystem::path texture_path;
    IDirect3DTexture9* texture = nullptr;
    bool sky = false;
    bool is_head = false;
};

struct Matrix4 {
    float m[16]{};
};

struct XaddSubobject {
    std::string code;
    std::string mesh_name;
    std::vector<std::string> texture_names;
};

struct SkinnedVertexSource {
    Vec3 position;
    Vec3 normal;
    float u = 0.0f;
    float v = 0.0f;
    std::uint8_t bone0 = 0;
    std::uint8_t bone1 = 0;
    float blend = 1.0f;
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
        return Vec3{0.0f, 0.0f, 1.0f};
    }
    return Vec3{value.x / length, value.y / length, value.z / length};
}

Quat normalize(Quat value) {
    const float length = std::sqrt(value.w * value.w + value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.00001f) {
        return Quat{};
    }
    return Quat{value.w / length, value.x / length, value.y / length, value.z / length};
}

Quat multiply(Quat a, Quat b) {
    return Quat{
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Vec3 rotate(Quat rotation, Vec3 value) {
    const Quat q = normalize(rotation);
    const Vec3 u{q.x, q.y, q.z};
    const float s = q.w;
    const Vec3 uv = cross(u, value);
    const Vec3 uuv = cross(u, uv);
    return Vec3{
        value.x + (uv.x * s + uuv.x) * 2.0f,
        value.y + (uv.y * s + uuv.y) * 2.0f,
        value.z + (uv.z * s + uuv.z) * 2.0f,
    };
}

D3DMATRIX identity_matrix() {
    D3DMATRIX matrix{};
    matrix._11 = 1.0f;
    matrix._22 = 1.0f;
    matrix._33 = 1.0f;
    matrix._44 = 1.0f;
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

Matrix4 matrix_identity4() {
    Matrix4 matrix{};
    matrix.m[0] = 1.0f;
    matrix.m[5] = 1.0f;
    matrix.m[10] = 1.0f;
    matrix.m[15] = 1.0f;
    return matrix;
}

Matrix4 matrix_from_skl_transform(const sphere::model::SklTransform& transform) {
    float x = transform.qx;
    float y = transform.qy;
    float z = transform.qz;
    float w = transform.qw;
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length <= 0.00001f) {
        x = 0.0f;
        y = 0.0f;
        z = 0.0f;
        w = 1.0f;
    } else {
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    }

    Matrix4 matrix = matrix_identity4();
    matrix.m[0] = 1.0f - 2.0f * y * y - 2.0f * z * z;
    matrix.m[1] = 2.0f * x * y + 2.0f * z * w;
    matrix.m[2] = 2.0f * x * z - 2.0f * y * w;
    matrix.m[4] = 2.0f * x * y - 2.0f * z * w;
    matrix.m[5] = 1.0f - 2.0f * x * x - 2.0f * z * z;
    matrix.m[6] = 2.0f * y * z + 2.0f * x * w;
    matrix.m[8] = 2.0f * x * z + 2.0f * y * w;
    matrix.m[9] = 2.0f * y * z - 2.0f * x * w;
    matrix.m[10] = 1.0f - 2.0f * x * x - 2.0f * y * y;
    matrix.m[12] = transform.tx;
    matrix.m[13] = transform.ty;
    matrix.m[14] = transform.tz;
    return matrix;
}

Matrix4 matrix_multiply(Matrix4 a, Matrix4 b) {
    Matrix4 out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out.m[row * 4 + col] =
                a.m[row * 4 + 0] * b.m[0 * 4 + col] +
                a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                a.m[row * 4 + 2] * b.m[2 * 4 + col] +
                a.m[row * 4 + 3] * b.m[3 * 4 + col];
        }
    }
    return out;
}

Vec3 transform_point(Matrix4 matrix, Vec3 value) {
    return Vec3{
        value.x * matrix.m[0] + value.y * matrix.m[4] + value.z * matrix.m[8] + matrix.m[12],
        value.x * matrix.m[1] + value.y * matrix.m[5] + value.z * matrix.m[9] + matrix.m[13],
        value.x * matrix.m[2] + value.y * matrix.m[6] + value.z * matrix.m[10] + matrix.m[14],
    };
}

Vec3 transform_vector(Matrix4 matrix, Vec3 value) {
    return Vec3{
        value.x * matrix.m[0] + value.y * matrix.m[4] + value.z * matrix.m[8],
        value.x * matrix.m[1] + value.y * matrix.m[5] + value.z * matrix.m[9],
        value.x * matrix.m[2] + value.y * matrix.m[6] + value.z * matrix.m[10],
    };
}

std::size_t skeleton_animation_frame_offset(const sphere::model::SklSkeleton& skeleton, std::size_t action) {
    if (action >= skeleton.animation_frame_counts.size()) {
        throw std::runtime_error("SKL has no requested animation action: " + skeleton.path.string());
    }
    std::size_t offset = 0;
    for (std::size_t i = 0; i < action; ++i) {
        offset += static_cast<std::size_t>(skeleton.animation_frame_counts[i]);
    }
    return offset;
}

std::size_t skeleton_bone_index(const sphere::model::SklSkeleton& skeleton, const std::string& name) {
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    const std::string wanted = lower(name);
    for (std::size_t i = 0; i < skeleton.bone_names.size(); ++i) {
        if (lower(skeleton.bone_names[i]) == wanted) {
            return i;
        }
    }
    throw std::runtime_error("SKL bone not found: " + name);
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
    if (data.size() < 128 || bin::u32le(data, 0) != 0x20534444) {
        throw std::runtime_error("bad DDS file: " + path.string());
    }
    if (bin::u32le(data, 4) != 124 || bin::u32le(data, 76) != 32) {
        throw std::runtime_error("bad DDS header: " + path.string());
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
    } else if ((pf_flags & 0x40U) != 0 &&
               rgb_bit_count == 32 &&
               r_mask == 0x00ff0000U &&
               g_mask == 0x0000ff00U &&
               b_mask == 0x000000ffU &&
               ((pf_flags & 0x1U) == 0 || a_mask == 0xff000000U)) {
        format = D3DFMT_A8R8G8B8;
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
            const std::uint32_t block_width = (std::max)(std::uint32_t{1}, (level_width + 3) / 4);
            const std::uint32_t block_height = (std::max)(std::uint32_t{1}, (level_height + 3) / 4);
            source_pitch = static_cast<std::size_t>(block_width) * block_bytes;
            source_rows = block_height;
        } else {
            source_pitch = static_cast<std::size_t>(level_width) * 4;
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

        const auto* source = data.data() + cursor;
        auto* dest = static_cast<std::uint8_t*>(locked.pBits);
        for (std::uint32_t row = 0; row < source_rows; ++row) {
            std::memcpy(dest + static_cast<std::size_t>(row) * locked.Pitch, source + row * source_pitch, source_pitch);
        }
        texture->UnlockRect(level);

        cursor += source_bytes;
        level_width = (std::max)(std::uint32_t{1}, level_width / 2);
        level_height = (std::max)(std::uint32_t{1}, level_height / 2);
    }
    return texture;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::filesystem::path model_material_texture_path(const std::filesystem::path& root, const std::string& material_name) {
    return root / "models" / "textures" / (lowercase_ascii(material_name) + ".dds");
}

std::vector<std::string> quoted_strings(const std::string& line) {
    std::vector<std::string> values;
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        const std::size_t open = line.find('"', cursor);
        if (open == std::string::npos) {
            break;
        }
        const std::size_t close = line.find('"', open + 1);
        if (close == std::string::npos) {
            throw std::runtime_error("unterminated quoted string in subobjs.dat");
        }
        values.push_back(line.substr(open + 1, close - open - 1));
        cursor = close + 1;
    }
    return values;
}

std::unordered_map<std::string, XaddSubobject> load_xadd_subobjects(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("failed to open subobjs.dat: " + path.string());
    }

    std::unordered_map<std::string, XaddSubobject> subobjects;
    std::string line;
    bool in_subobjects = false;
    while (std::getline(file, line)) {
        if (line.find("subobjs<as>") != std::string::npos) {
            in_subobjects = true;
            continue;
        }
        if (in_subobjects && line.find("lods<as>") != std::string::npos) {
            break;
        }
        if (!in_subobjects) {
            continue;
        }
        if (line.find('{') == std::string::npos || line.find("s<t>=") == std::string::npos) {
            continue;
        }

        const auto values = quoted_strings(line);
        if (values.size() < 3) {
            throw std::runtime_error("bad subobjs.dat entry: " + line);
        }

        XaddSubobject entry;
        entry.code = values[0];
        entry.mesh_name = values[1];
        entry.texture_names.assign(values.begin() + 2, values.end());
        subobjects[entry.code] = std::move(entry);
    }

    if (subobjects.empty()) {
        throw std::runtime_error("subobjs.dat has no subobjects: " + path.string());
    }
    return subobjects;
}

const std::vector<std::string>& face_codes(bool female) {
    static const std::vector<std::string> male = {
        "mf0", "mf1", "mf2", "mf3", "mf4", "mf5", "mf6", "mf7", "mf8", "mf9", "mfa", "mfb", "mfc",
    };
    static const std::vector<std::string> female_codes = {
        "wf0", "wf1", "wf2", "wf3", "wf4", "wf5", "wf6", "wf7", "wf8", "wf9", "wfa", "wfb",
    };
    return female ? female_codes : male;
}

const std::vector<std::string>& hair_codes(bool female) {
    static const std::vector<std::string> male = {"mr0", "mr1", "mr2"};
    static const std::vector<std::string> female_codes = {"wr0", "wr1", "wr2", "wr3", "wr4"};
    return female ? female_codes : male;
}

std::size_t checked_pattern_index(int value, std::size_t count, const char* label) {
    if (value < 0 || static_cast<std::size_t>(value) >= count) {
        std::ostringstream out;
        out << "character " << label << " index out of range: " << value << " / " << count;
        throw std::runtime_error(out.str());
    }
    return static_cast<std::size_t>(value);
}

std::vector<std::string> character_subobject_codes(bool female, int face, int hair) {
    const auto& faces = face_codes(female);
    const auto& hairs = hair_codes(female);
    const std::size_t face_index = checked_pattern_index(face, faces.size(), "face");
    const std::size_t hair_index = checked_pattern_index(hair, hairs.size(), "hair");

    if (female) {
        return {"wb0", "wt0", "wg0", "wc0", "we0", faces[face_index], hairs[hair_index]};
    }
    return {"mb0", "mt0", "mg0", "mc0", "me0", faces[face_index], hairs[hair_index]};
}

bool is_face_code(const std::string& code) {
    return code.size() == 3 &&
        ((code[0] == 'm' && code[1] == 'f') ||
         (code[0] == 'w' && code[1] == 'f'));
}

bool is_hair_code(const std::string& code) {
    return code.size() == 3 &&
        ((code[0] == 'm' && code[1] == 'r') ||
         (code[0] == 'w' && code[1] == 'r'));
}

std::size_t texture_index_for_subobject(const XaddSubobject& entry, int hair_color, int tattoo) {
    std::size_t texture_index = 0;
    if (is_face_code(entry.code)) {
        texture_index = checked_pattern_index(tattoo, entry.texture_names.size(), "tattoo");
    } else if (is_hair_code(entry.code)) {
        texture_index = checked_pattern_index(hair_color, entry.texture_names.size(), "hair color");
    }
    if (texture_index >= entry.texture_names.size()) {
        throw std::runtime_error("subobject texture index out of range: " + entry.code);
    }
    return texture_index;
}

std::vector<Matrix4> build_skeleton_matrices(const sphere::model::SklSkeleton& skeleton, std::size_t frame) {
    if (frame >= static_cast<std::size_t>(skeleton.frame_count)) {
        throw std::runtime_error("SKL frame out of range: " + skeleton.path.string());
    }

    const auto bone_count = static_cast<std::size_t>(skeleton.bone_count);
    std::vector<Matrix4> matrices(bone_count);
    std::vector<std::uint8_t> states(bone_count, 0);

    auto resolve = [&](auto&& self, std::size_t bone) -> Matrix4 {
        if (states[bone] == 2) {
            return matrices[bone];
        }
        if (states[bone] == 1) {
            throw std::runtime_error("SKL parent hierarchy has a cycle: " + skeleton.path.string());
        }
        states[bone] = 1;

        const auto& transform = skeleton.transforms[frame * bone_count + bone];
        Matrix4 matrix = matrix_from_skl_transform(transform);
        const auto parent = skeleton.parents[bone];
        if (parent >= 0) {
            matrix = matrix_multiply(matrix, self(self, static_cast<std::size_t>(parent)));
        }

        matrices[bone] = matrix;
        states[bone] = 2;
        return matrix;
    };

    for (std::size_t i = 0; i < bone_count; ++i) {
        resolve(resolve, i);
    }
    return matrices;
}

std::vector<std::uint8_t> build_chr_to_skl_bone_map(
    const sphere::model::ChrMesh& mesh,
    const sphere::model::SklSkeleton& skeleton) {
    std::unordered_map<std::string, std::uint8_t> skeleton_bones;
    for (std::size_t i = 0; i < skeleton.bone_names.size(); ++i) {
        if (i > 0xff) {
            throw std::runtime_error("SKL has too many bones for CHR mapping: " + skeleton.path.string());
        }
        skeleton_bones[lowercase_ascii(skeleton.bone_names[i])] = static_cast<std::uint8_t>(i);
    }

    std::vector<std::uint8_t> remap;
    remap.reserve(mesh.info.bone_names.size());
    for (const auto& name : mesh.info.bone_names) {
        const auto it = skeleton_bones.find(lowercase_ascii(name));
        if (it == skeleton_bones.end()) {
            throw std::runtime_error("CHR bone '" + name + "' not found in " + skeleton.path.string());
        }
        remap.push_back(it->second);
    }
    return remap;
}

} // namespace

struct CharacterSelectScene::Impl {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexBuffer9* vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* index_buffer = nullptr;
    IDirect3DVertexBuffer9* ground_vertex_buffer = nullptr;
    IDirect3DIndexBuffer9* ground_index_buffer = nullptr;
    IDirect3DTexture9* overlay_texture = nullptr;
    D3DPRESENT_PARAMETERS present{};
    std::vector<SceneVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<SceneBatch> character_batches;
    std::vector<SkinnedVertexSource> character_sources;
    sphere::model::SklSkeleton character_skeleton;
    std::size_t character_root_bone = 0;
    Vec3 character_root_bind_position;
    float character_center_x = 0.0f;
    float character_min_y = 0.0f;
    float character_center_z = 0.0f;
    float character_scale = 1.0f;
    std::size_t character_animation_start = 0;
    std::size_t character_animation_frames = 1;
    DWORD character_animation_tick = 0;
    std::vector<SceneVertex> ground_vertices;
    std::vector<std::uint16_t> ground_indices;
    std::vector<SceneBatch> ground_batches;
    std::filesystem::path root_path;
    bool female_character = false;
    int face_index = 0;
    int hair_index = 0;
    int hair_color_index = 0;
    int tattoo_index = 0;
    float angle = 3.1415926535f;
    CharacterCameraProfileTable camera_profiles{};
    float camera_focus_x = 0.0f;
    float camera_focus_x_target = 0.0f;
    float camera_focus_y = 1.34f;
    float camera_focus_y_target = 1.34f;
    float camera_focus_z = 0.0f;
    float camera_focus_z_target = 0.0f;
    float camera_yaw = 0.0f;
    float camera_yaw_target = 0.0f;
    float camera_distance = 4.45f;
    float camera_distance_target = 4.45f;
    float camera_pitch = 0.02f;
    float camera_pitch_target = 0.02f;
    float camera_fov_degrees = 50.0f;
    float camera_fov_degrees_target = 50.0f;
    DWORD start_tick = 0;
    int overlay_width = 0;
    int overlay_height = 0;
    int overlay_x = 0;
    int overlay_y = 0;
    bool overlay_align_right_x = false;
    bool initialized = false;

    ~Impl() {
        release();
    }

    void release() {
        release_com(overlay_texture);
        for (auto& batch : character_batches) {
            release_com(batch.texture);
        }
        for (auto& batch : ground_batches) {
            release_com(batch.texture);
        }
        release_com(ground_index_buffer);
        release_com(ground_vertex_buffer);
        release_com(index_buffer);
        release_com(vertex_buffer);
        release_com(device);
        release_com(d3d);
        initialized = false;
    }

    RECT client_rect() const {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (rc.right <= rc.left) {
            rc.right = rc.left + 1;
        }
        if (rc.bottom <= rc.top) {
            rc.bottom = rc.top + 1;
        }
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
        const DWORD flags[] = {
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        };
        const D3DFORMAT depth_formats[] = {
            D3DFMT_D24S8,
            D3DFMT_D16,
        };

        HRESULT last_hr = E_FAIL;
        for (D3DFORMAT depth : depth_formats) {
            present.AutoDepthStencilFormat = depth;
            for (DWORD flag : flags) {
                last_hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flag, &present, &device);
                if (SUCCEEDED(last_hr)) {
                    return true;
                }
            }
        }

        error = hresult_text("CreateDevice", last_hr);
        return false;
    }

    void build_material_batches(
        const sphere::model::MdlMesh& mesh,
        const std::filesystem::path& root,
        const std::string& asset_name,
        std::vector<std::uint16_t>& out_indices,
        std::vector<SceneBatch>& out_batches) {
        if (mesh.info.materials.empty()) {
            throw std::runtime_error(asset_name + " has no material names");
        }
        if (mesh.surfaces.empty()) {
            throw std::runtime_error(asset_name + " has no surfaces");
        }

        out_indices.clear();
        out_batches.clear();
        const std::size_t material_count = mesh.info.materials.size();
        std::vector<std::vector<std::uint16_t>> indices_by_material(material_count);

        for (const auto& surface : mesh.surfaces) {
            if (surface.first_triangle_index < 0 || surface.triangle_count < 0 ||
                surface.first_vertex_index < 0 || surface.vertex_count < 0) {
                throw std::runtime_error(asset_name + " has negative surface ranges");
            }

            const std::size_t texture_index = surface.texture_index;
            if (texture_index >= material_count) {
                throw std::runtime_error(asset_name + " references missing surface texture index");
            }

            const std::size_t first_triangle = static_cast<std::size_t>(surface.first_triangle_index);
            const std::size_t triangle_count = static_cast<std::size_t>(surface.triangle_count);
            const std::size_t first_vertex = static_cast<std::size_t>(surface.first_vertex_index);
            const std::size_t vertex_count = static_cast<std::size_t>(surface.vertex_count);
            if (first_triangle > mesh.triangles.size() || triangle_count > mesh.triangles.size() - first_triangle) {
                throw std::runtime_error(asset_name + " surface triangle range is outside triangle table");
            }
            if (first_vertex > mesh.vertices.size() || vertex_count > mesh.vertices.size() - first_vertex) {
                throw std::runtime_error(asset_name + " surface vertex range is outside vertex table");
            }

            auto& group = indices_by_material[texture_index];
            for (std::size_t i = 0; i < triangle_count; ++i) {
                const auto& triangle = mesh.triangles[first_triangle + i];
                if (triangle.a >= vertex_count || triangle.b >= vertex_count || triangle.c >= vertex_count) {
                    throw std::runtime_error(asset_name + " surface triangle uses vertex outside surface range");
                }
                group.push_back(static_cast<std::uint16_t>(first_vertex + triangle.a));
                group.push_back(static_cast<std::uint16_t>(first_vertex + triangle.b));
                group.push_back(static_cast<std::uint16_t>(first_vertex + triangle.c));
            }
        }

        for (std::size_t material = 0; material < indices_by_material.size(); ++material) {
            auto& group = indices_by_material[material];
            if (group.empty()) {
                continue;
            }
            const UINT start_index = static_cast<UINT>(out_indices.size());
            out_indices.insert(out_indices.end(), group.begin(), group.end());
            const auto texture = model_material_texture_path(root, mesh.info.materials[material]);
            if (!std::filesystem::exists(texture)) {
                throw std::runtime_error("missing " + asset_name + " material texture: " + texture.string());
            }
            out_batches.push_back(SceneBatch{
                start_index,
                static_cast<UINT>(group.size()),
                texture,
                nullptr,
                mesh.info.materials[material] == "LOAD_SC02",
            });
        }
        if (out_batches.empty()) {
            throw std::runtime_error(asset_name + " has no material batches");
        }
    }

    std::size_t action_frame_offset(const sphere::model::MdlMesh& mesh, std::size_t action, std::size_t frame, const std::string& asset_name) {
        if (action >= mesh.actions.size()) {
            throw std::runtime_error(asset_name + " has no requested action");
        }
        if (frame >= mesh.actions[action]) {
            throw std::runtime_error(asset_name + " has no requested action frame");
        }

        std::size_t offset = frame;
        for (std::size_t i = 0; i < action; ++i) {
            offset += mesh.actions[i];
        }
        return offset;
    }

    ObjectPose local_object_pose(
        const sphere::model::MdlMesh& mesh,
        std::size_t object_index,
        std::size_t animation_frame_offset,
        const std::string& asset_name) {
        const auto& object = mesh.objects[object_index];
        if (object.key_index < 0) {
            throw std::runtime_error(asset_name + " object '" + object.name + "' references missing transform key");
        }
        std::size_t key_index = static_cast<std::size_t>(object.key_index);
        if (object.is_animated != 0) {
            key_index += animation_frame_offset;
        }
        if (key_index >= mesh.transform_keys.size()) {
            throw std::runtime_error(asset_name + " object '" + object.name + "' references missing transform key");
        }
        const auto& key = mesh.transform_keys[key_index];
        return ObjectPose{
            Vec3{key.x, key.y, key.z},
            normalize(Quat{key.qw, key.qx, key.qy, key.qz}),
        };
    }

    std::vector<ObjectPose> build_object_poses(const sphere::model::MdlMesh& mesh, const std::string& asset_name) {
        if (mesh.objects.empty()) {
            throw std::runtime_error(asset_name + " has no object table");
        }
        if (mesh.transform_keys.empty()) {
            throw std::runtime_error(asset_name + " has no transform keys");
        }
        if (mesh.actions.empty()) {
            throw std::runtime_error(asset_name + " has no action table");
        }
        const std::size_t animation_frame_offset = action_frame_offset(mesh, kCharacterFreeAction, 0, asset_name);

        std::vector<int> parents(mesh.objects.size(), -1);
        for (std::size_t parent = 0; parent < mesh.objects.size(); ++parent) {
            const auto& object = mesh.objects[parent];
            const std::size_t offset = object.object_index_offset;
            const std::size_t count = object.connected_bone_count;
            if (offset > mesh.object_indices.size() || count > mesh.object_indices.size() - offset) {
                throw std::runtime_error(asset_name + " object '" + object.name + "' has invalid child index range");
            }
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t child = mesh.object_indices[offset + i];
                if (child >= mesh.objects.size()) {
                    throw std::runtime_error(asset_name + " object '" + object.name + "' references missing child object");
                }
                parents[child] = static_cast<int>(parent);
            }
        }

        std::vector<ObjectPose> poses(mesh.objects.size());
        std::vector<std::uint8_t> states(mesh.objects.size(), 0);
        auto resolve = [&](auto&& self, std::size_t index) -> ObjectPose {
            if (states[index] == 2) {
                return poses[index];
            }
            if (states[index] == 1) {
                throw std::runtime_error(asset_name + " object hierarchy has a cycle");
            }
            states[index] = 1;

            ObjectPose pose = local_object_pose(mesh, index, animation_frame_offset, asset_name);
            const bool dummy_parent = parents[index] == 0 && !mesh.objects.empty() && mesh.objects[0].name == "Dummy01";
            if (parents[index] >= 0 && !dummy_parent) {
                const ObjectPose parent = self(self, static_cast<std::size_t>(parents[index]));
                pose.position = Vec3{
                    parent.position.x + rotate(parent.rotation, pose.position).x,
                    parent.position.y + rotate(parent.rotation, pose.position).y,
                    parent.position.z + rotate(parent.rotation, pose.position).z,
                };
                pose.rotation = normalize(multiply(parent.rotation, pose.rotation));
            }

            poses[index] = pose;
            states[index] = 2;
            return pose;
        };

        for (std::size_t i = 0; i < mesh.objects.size(); ++i) {
            resolve(resolve, i);
        }
        return poses;
    }

    void update_character_vertices_for_frame(std::size_t frame) {
        if (character_sources.empty()) {
            return;
        }
        auto skeleton_matrices = build_skeleton_matrices(character_skeleton, frame);
        if (character_root_bone >= skeleton_matrices.size()) {
            throw std::runtime_error("character root bone index out of range");
        }
        const Vec3 root_delta{
            skeleton_matrices[character_root_bone].m[12] - character_root_bind_position.x,
            skeleton_matrices[character_root_bone].m[13] - character_root_bind_position.y,
            skeleton_matrices[character_root_bone].m[14] - character_root_bind_position.z,
        };
        for (auto& matrix : skeleton_matrices) {
            matrix.m[12] -= root_delta.x;
            matrix.m[13] -= root_delta.y;
            matrix.m[14] -= root_delta.z;
        }
        vertices.resize(character_sources.size());
        for (std::size_t i = 0; i < character_sources.size(); ++i) {
            const auto& source = character_sources[i];
            if (source.bone0 >= skeleton_matrices.size() || source.bone1 >= skeleton_matrices.size()) {
                throw std::runtime_error("skinned character source bone index out of range");
            }
            const Matrix4 matrix0 = skeleton_matrices[source.bone0];
            const Matrix4 matrix1 = skeleton_matrices[source.bone1];
            const float weight0 = std::clamp(source.blend, 0.0f, 1.0f);
            const float weight1 = 1.0f - weight0;
            const Vec3 p0 = transform_point(matrix0, source.position);
            const Vec3 p1 = transform_point(matrix1, source.position);
            const Vec3 n0 = transform_vector(matrix0, source.normal);
            const Vec3 n1 = transform_vector(matrix1, source.normal);
            const Vec3 skinned_position{
                p0.x * weight0 + p1.x * weight1,
                p0.y * weight0 + p1.y * weight1,
                p0.z * weight0 + p1.z * weight1,
            };
            const Vec3 skinned_normal = normalize(Vec3{
                n0.x * weight0 + n1.x * weight1,
                n0.y * weight0 + n1.y * weight1,
                n0.z * weight0 + n1.z * weight1,
            });

            SceneVertex vertex;
            vertex.x = (skinned_position.x - character_center_x) * character_scale;
            vertex.y = ((-skinned_position.y) - character_min_y) * character_scale;
            vertex.z = (skinned_position.z - character_center_z) * character_scale;
            const Vec3 normal = normalize(Vec3{skinned_normal.x, -skinned_normal.y, skinned_normal.z});
            vertex.nx = normal.x;
            vertex.ny = normal.y;
            vertex.nz = normal.z;
            vertex.diffuse = 0xffffffff;
            vertex.u = source.u;
            vertex.v = source.v;
            vertices[i] = vertex;
        }
    }

    void load_character_mesh(const std::filesystem::path& root, bool female, int face, int hair, int hair_color, int tattoo) {
        const auto skeleton_path = root / "xadd" / (female ? L"woman.skl" : L"man.skl");
        character_skeleton = sphere::model::load_skl_skeleton(skeleton_path);
        character_animation_start = skeleton_animation_frame_offset(character_skeleton, kCharacterFreeAction);
        character_animation_frames = static_cast<std::size_t>(character_skeleton.animation_frame_counts[kCharacterFreeAction]);
        character_animation_tick = GetTickCount();

        const auto animation_origin_matrices = build_skeleton_matrices(character_skeleton, character_animation_start);
        character_root_bone = skeleton_bone_index(character_skeleton, "hips");
        character_root_bind_position = Vec3{
            animation_origin_matrices[character_root_bone].m[12],
            animation_origin_matrices[character_root_bone].m[13],
            animation_origin_matrices[character_root_bone].m[14],
        };
        const auto subobjects = load_xadd_subobjects(root / "xadd" / "subobjs.dat");
        const auto codes = character_subobject_codes(female, face, hair);
        // The last two subobject codes are the face and hair meshes (the head).
        std::set<std::string> head_codes;
        if (codes.size() >= 2) {
            head_codes.insert(codes[codes.size() - 1]);
            head_codes.insert(codes[codes.size() - 2]);
        }

        vertices.clear();
        indices.clear();
        character_batches.clear();
        character_sources.clear();

        bool have_bounds = false;
        float min_x = 0.0f;
        float max_x = 0.0f;
        float min_y = 0.0f;
        float max_y = 0.0f;
        float min_z = 0.0f;
        float max_z = 0.0f;

        for (const auto& code : codes) {
            const auto entry_it = subobjects.find(code);
            if (entry_it == subobjects.end()) {
                throw std::runtime_error("missing subobject code in subobjs.dat: " + code);
            }
            const auto& entry = entry_it->second;
            if (entry.texture_names.empty()) {
                throw std::runtime_error("subobject has no textures: " + code);
            }
            const std::size_t texture_index = texture_index_for_subobject(entry, hair_color, tattoo);

            const auto chr_path = root / "xadd" / (entry.mesh_name + ".chr");
            if (!std::filesystem::exists(chr_path)) {
                throw std::runtime_error("missing CHR mesh: " + chr_path.string());
            }
            const auto mesh = sphere::model::load_chr_mesh(chr_path);
            if (mesh.vertices.empty() || mesh.indices.empty()) {
                throw std::runtime_error("CHR mesh has no renderable triangles: " + chr_path.string());
            }
            const auto bone_remap = build_chr_to_skl_bone_map(mesh, character_skeleton);

            const auto texture_path = model_material_texture_path(root, entry.texture_names[texture_index]);
            if (!std::filesystem::exists(texture_path)) {
                throw std::runtime_error("missing character texture: " + texture_path.string());
            }

            const std::size_t vertex_base = character_sources.size();
            if (vertex_base + mesh.vertices.size() > 0xffff) {
                throw std::runtime_error("combined character mesh exceeds 16-bit index range");
            }
            character_sources.reserve(character_sources.size() + mesh.vertices.size());
            for (const auto& source : mesh.vertices) {
                if (source.bone0 >= bone_remap.size() || source.bone1 >= bone_remap.size()) {
                    throw std::runtime_error("CHR vertex bone index out of range: " + chr_path.string());
                }
                const auto bone0_index = bone_remap[source.bone0];
                const auto bone1_index = bone_remap[source.bone1];
                if (bone0_index >= animation_origin_matrices.size() || bone1_index >= animation_origin_matrices.size()) {
                    throw std::runtime_error("CHR remapped bone index out of range: " + chr_path.string());
                }
                character_sources.push_back(SkinnedVertexSource{
                    Vec3{source.x, source.y, source.z},
                    Vec3{source.nx, source.ny, source.nz},
                    source.u,
                    source.v,
                    bone0_index,
                    bone1_index,
                    source.blend,
                });

                const float bound_x = source.x;
                const float bound_y = -source.y;
                const float bound_z = source.z;
                if (!have_bounds) {
                    min_x = max_x = bound_x;
                    min_y = max_y = bound_y;
                    min_z = max_z = bound_z;
                    have_bounds = true;
                } else {
                    min_x = (std::min)(min_x, bound_x);
                    max_x = (std::max)(max_x, bound_x);
                    min_y = (std::min)(min_y, bound_y);
                    max_y = (std::max)(max_y, bound_y);
                    min_z = (std::min)(min_z, bound_z);
                    max_z = (std::max)(max_z, bound_z);
                }
            }

            const UINT start_index = static_cast<UINT>(indices.size());
            indices.reserve(indices.size() + mesh.indices.size());
            for (const auto index : mesh.indices) {
                indices.push_back(static_cast<std::uint16_t>(vertex_base + index));
            }
            character_batches.push_back(SceneBatch{
                start_index,
                static_cast<UINT>(mesh.indices.size()),
                texture_path,
                nullptr,
                false,
                head_codes.count(code) != 0,
            });
        }

        if (!have_bounds || indices.empty()) {
            throw std::runtime_error("XADD character has no renderable geometry");
        }

        character_center_x = (min_x + max_x) * 0.5f;
        character_center_z = (min_z + max_z) * 0.5f;
        character_min_y = min_y;
        const float span_y = max_y - min_y;
        character_scale = 2.05f / (std::max)(span_y, 0.001f);
        update_character_vertices_for_frame(character_animation_start);
    }

    void load_mdl_scene_mesh(const std::filesystem::path& root) {
        const auto mdl_path = root / "models" / "loadscene.mdl";
        const auto mesh = sphere::model::load_mdl_mesh(mdl_path);
        if (mesh.vertices.empty() || mesh.triangles.empty()) {
            throw std::runtime_error("loadscene.mdl has no renderable triangles");
        }
        if (mesh.info.materials.empty()) {
            throw std::runtime_error("loadscene.mdl has no material names");
        }

        bool have_scene_bounds = false;
        float min_x = 0.0f;
        float max_x = 0.0f;
        float min_y = 0.0f;
        float max_y = 0.0f;
        float min_z = 0.0f;
        float max_z = 0.0f;
        for (const auto& surface : mesh.surfaces) {
            if (surface.texture_index < mesh.info.materials.size() && mesh.info.materials[surface.texture_index] == "LOAD_SC02") {
                continue;
            }
            if (surface.first_vertex_index < 0 || surface.vertex_count <= 0) {
                throw std::runtime_error("loadscene.mdl has invalid surface vertex range");
            }
            const auto first_vertex = static_cast<std::size_t>(surface.first_vertex_index);
            const auto vertex_count = static_cast<std::size_t>(surface.vertex_count);
            if (first_vertex > mesh.vertices.size() || vertex_count > mesh.vertices.size() - first_vertex) {
                throw std::runtime_error("loadscene.mdl surface vertex range is outside vertex table");
            }
            for (std::size_t i = 0; i < vertex_count; ++i) {
                const auto& source = mesh.vertices[first_vertex + i];
                const float scene_x = source.z;
                const float scene_y = -source.y;
                const float scene_z = source.x;
                if (!have_scene_bounds) {
                    min_x = max_x = scene_x;
                    min_y = max_y = scene_y;
                    min_z = max_z = scene_z;
                    have_scene_bounds = true;
                } else {
                    min_x = (std::min)(min_x, scene_x);
                    max_x = (std::max)(max_x, scene_x);
                    min_y = (std::min)(min_y, scene_y);
                    max_y = (std::max)(max_y, scene_y);
                    min_z = (std::min)(min_z, scene_z);
                    max_z = (std::max)(max_z, scene_z);
                }
            }
        }
        if (!have_scene_bounds) {
            throw std::runtime_error("loadscene.mdl has no non-sky scene bounds");
        }

        const float center_x = (min_x + max_x) * 0.5f;
        const float center_z = (min_z + max_z) * 0.5f;
        const float span_x = max_x - min_x;
        const float span_y = max_y - min_y;
        const float span_z = max_z - min_z;
        const float scale = 8.8f / std::max({span_x, span_y, span_z, 0.001f});
        const DWORD color = D3DCOLOR_ARGB(255, 220, 214, 196);

        ground_vertices.clear();
        ground_vertices.reserve(mesh.vertices.size());
        for (const auto& source : mesh.vertices) {
            const Vec3 normal = normalize(Vec3{source.nz, -source.ny, source.nx});
            SceneVertex vertex;
            vertex.x = (source.z - center_x) * scale;
            vertex.y = ((-source.y) - min_y) * scale - 0.03f;
            vertex.z = (source.x - center_z) * scale;
            vertex.nx = normal.x;
            vertex.ny = normal.y;
            vertex.nz = normal.z;
            vertex.diffuse = color;
            vertex.u = source.u;
            vertex.v = source.v;
            ground_vertices.push_back(vertex);
        }

        build_material_batches(mesh, root, "loadscene.mdl", ground_indices, ground_batches);
    }

    void load_ground_mesh(const std::filesystem::path& root) {
        ground_vertices.clear();
        ground_indices.clear();
        ground_batches.clear();
        load_mdl_scene_mesh(root);
    }

    bool upload_buffers(std::wstring& error) {
        try {
            for (auto& batch : character_batches) {
                batch.texture = load_dds_texture(device, batch.texture_path);
            }
            for (auto& batch : ground_batches) {
                batch.texture = load_dds_texture(device, batch.texture_path);
            }
        } catch (const std::exception& ex) {
            std::ostringstream out;
            out << "DDS load failed: " << ex.what();
            assign_error(error, out.str());
            return false;
        }

        const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(SceneVertex));
        HRESULT hr = device->CreateVertexBuffer(vertex_bytes, 0, kSceneVertexFvf, D3DPOOL_MANAGED, &vertex_buffer, nullptr);
        if (FAILED(hr)) {
            error = hresult_text("CreateVertexBuffer", hr);
            return false;
        }

        void* vertex_data = nullptr;
        hr = vertex_buffer->Lock(0, vertex_bytes, &vertex_data, 0);
        if (FAILED(hr)) {
            error = hresult_text("VertexBuffer::Lock", hr);
            return false;
        }
        std::memcpy(vertex_data, vertices.data(), vertex_bytes);
        vertex_buffer->Unlock();

        const UINT index_bytes = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        hr = device->CreateIndexBuffer(index_bytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &index_buffer, nullptr);
        if (FAILED(hr)) {
            error = hresult_text("CreateIndexBuffer", hr);
            return false;
        }

        void* index_data = nullptr;
        hr = index_buffer->Lock(0, index_bytes, &index_data, 0);
        if (FAILED(hr)) {
            error = hresult_text("IndexBuffer::Lock", hr);
            return false;
        }
        std::memcpy(index_data, indices.data(), index_bytes);
        index_buffer->Unlock();

        const UINT ground_vertex_bytes = static_cast<UINT>(ground_vertices.size() * sizeof(SceneVertex));
        hr = device->CreateVertexBuffer(ground_vertex_bytes, 0, kSceneVertexFvf, D3DPOOL_MANAGED, &ground_vertex_buffer, nullptr);
        if (FAILED(hr)) {
            error = hresult_text("CreateVertexBuffer ground", hr);
            return false;
        }

        vertex_data = nullptr;
        hr = ground_vertex_buffer->Lock(0, ground_vertex_bytes, &vertex_data, 0);
        if (FAILED(hr)) {
            error = hresult_text("GroundVertexBuffer::Lock", hr);
            return false;
        }
        std::memcpy(vertex_data, ground_vertices.data(), ground_vertex_bytes);
        ground_vertex_buffer->Unlock();

        const UINT ground_index_bytes = static_cast<UINT>(ground_indices.size() * sizeof(std::uint16_t));
        hr = device->CreateIndexBuffer(ground_index_bytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ground_index_buffer, nullptr);
        if (FAILED(hr)) {
            error = hresult_text("CreateIndexBuffer ground", hr);
            return false;
        }

        index_data = nullptr;
        hr = ground_index_buffer->Lock(0, ground_index_bytes, &index_data, 0);
        if (FAILED(hr)) {
            error = hresult_text("GroundIndexBuffer::Lock", hr);
            return false;
        }
        std::memcpy(index_data, ground_indices.data(), ground_index_bytes);
        ground_index_buffer->Unlock();
        return true;
    }

    void configure_render_state() {
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_LIGHTING, TRUE);
        device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(50, 44, 40));
        device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
        device->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
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
        device->SetRenderState(D3DRS_FOGCOLOR, D3DCOLOR_XRGB(16, 17, 16));
        device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        const float fog_start = 7.0f;
        const float fog_end = 16.0f;
        DWORD fog_start_bits = 0;
        DWORD fog_end_bits = 0;
        std::memcpy(&fog_start_bits, &fog_start, sizeof(fog_start));
        std::memcpy(&fog_end_bits, &fog_end, sizeof(fog_end));
        device->SetRenderState(D3DRS_FOGSTART, fog_start_bits);
        device->SetRenderState(D3DRS_FOGEND, fog_end_bits);

        D3DMATERIAL9 material{};
        material.Diffuse.r = 1.0f;
        material.Diffuse.g = 1.0f;
        material.Diffuse.b = 1.0f;
        material.Diffuse.a = 1.0f;
        material.Ambient = material.Diffuse;
        device->SetMaterial(&material);

        D3DLIGHT9 light{};
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse.r = 1.0f;
        light.Diffuse.g = 0.92f;
        light.Diffuse.b = 0.82f;
        light.Ambient.r = 0.28f;
        light.Ambient.g = 0.25f;
        light.Ambient.b = 0.22f;
        light.Direction.x = -0.35f;
        light.Direction.y = -0.55f;
        light.Direction.z = 0.75f;
        device->SetLight(0, &light);
        device->LightEnable(0, TRUE);
    }

    static float approach(float current, float target, float factor) {
        return current + (target - current) * factor;
    }

    void set_camera_profiles(const CharacterCameraProfileTable& profiles) {
        camera_profiles = profiles;
    }

    void set_camera_focus(int focus_id) {
        if (focus_id < 0 || focus_id >= static_cast<int>(camera_profiles.size())) {
            return;
        }
        const auto& profile = camera_profiles[static_cast<std::size_t>(focus_id)];
        if (!profile.valid) {
            return;
        }
        camera_focus_x_target = profile.target_x;
        camera_focus_y_target = profile.target_y;
        camera_focus_z_target = profile.target_z;
        camera_yaw_target = profile.yaw;
        camera_distance_target = profile.distance;
        camera_pitch_target = profile.pitch;
        camera_fov_degrees_target = profile.fov_degrees;
    }

    void update_camera() {
        camera_focus_x = approach(camera_focus_x, camera_focus_x_target, 0.12f);
        camera_focus_y = approach(camera_focus_y, camera_focus_y_target, 0.12f);
        camera_focus_z = approach(camera_focus_z, camera_focus_z_target, 0.12f);
        camera_yaw = approach(camera_yaw, camera_yaw_target, 0.12f);
        camera_distance = approach(camera_distance, camera_distance_target, 0.12f);
        camera_pitch = approach(camera_pitch, camera_pitch_target, 0.12f);
        camera_fov_degrees = approach(camera_fov_degrees, camera_fov_degrees_target, 0.12f);
    }

    void snap_camera_to_target() {
        camera_focus_x = camera_focus_x_target;
        camera_focus_y = camera_focus_y_target;
        camera_focus_z = camera_focus_z_target;
        camera_yaw = camera_yaw_target;
        camera_distance = camera_distance_target;
        camera_pitch = camera_pitch_target;
        camera_fov_degrees = camera_fov_degrees_target;
    }

    void update_view_projection() {
        const RECT rc = client_rect();
        const float width = static_cast<float>(rc.right - rc.left);
        const float height = static_cast<float>(rc.bottom - rc.top);
        const float aspect = width / (std::max)(height, 1.0f);

        const Vec3 target{camera_focus_x, camera_focus_y, camera_focus_z};
        const float cp = std::cos(camera_pitch);
        const float sp = std::sin(camera_pitch);
        const float sy = std::sin(camera_yaw);
        const float cy = std::cos(camera_yaw);
        const float horizontal_distance = cp * camera_distance;
        const Vec3 eye{
            target.x + sy * horizontal_distance,
            target.y + sp * camera_distance,
            target.z - cy * horizontal_distance,
        };
        const D3DMATRIX view = look_at_rh_matrix(eye, target, Vec3{0.0f, 1.0f, 0.0f});
        const D3DMATRIX projection = perspective_fov_rh_matrix(camera_fov_degrees * 3.1415926535f / 180.0f, aspect, 0.05f, 100.0f);

        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
    }

    void draw_ground() {
        if (!ground_vertex_buffer || !ground_index_buffer || ground_batches.empty()) {
            return;
        }
        const D3DMATRIX world = identity_matrix();
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetFVF(kSceneVertexFvf);
        device->SetStreamSource(0, ground_vertex_buffer, 0, sizeof(SceneVertex));
        device->SetIndices(ground_index_buffer);

        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        for (const auto& batch : ground_batches) {
            if (!batch.sky || !batch.texture || batch.index_count < 3) {
                continue;
            }
            device->SetTexture(0, batch.texture);
            device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,
                0,
                0,
                static_cast<UINT>(ground_vertices.size()),
                batch.start_index,
                batch.index_count / 3);
        }

        configure_render_state();
        device->SetFVF(kSceneVertexFvf);
        device->SetStreamSource(0, ground_vertex_buffer, 0, sizeof(SceneVertex));
        device->SetIndices(ground_index_buffer);
        for (const auto& batch : ground_batches) {
            if (batch.sky || !batch.texture || batch.index_count < 3) {
                continue;
            }
            device->SetTexture(0, batch.texture);
            device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,
                0,
                0,
                static_cast<UINT>(ground_vertices.size()),
                batch.start_index,
                batch.index_count / 3);
        }
    }

    void draw_character() {
        if (!vertex_buffer || !index_buffer || character_batches.empty()) {
            return;
        }
        D3DMATRIX world = rotation_y_matrix(angle);
        world._42 = 0.56f;
        world._43 = -0.18f;
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetFVF(kSceneVertexFvf);
        device->SetStreamSource(0, vertex_buffer, 0, sizeof(SceneVertex));
        device->SetIndices(index_buffer);
        for (const auto& batch : character_batches) {
            if (!batch.texture || batch.index_count < 3) {
                continue;
            }
            device->SetTexture(0, batch.texture);
            device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,
                0,
                0,
                static_cast<UINT>(vertices.size()),
                batch.start_index,
                batch.index_count / 3);
        }
    }

    CharacterRenderMesh character_render_mesh() const {
        CharacterRenderMesh out;
        out.vertices.reserve(vertices.size());
        for (const auto& vertex : vertices) {
            out.vertices.push_back(CharacterRenderVertex{
                vertex.x,
                vertex.y,
                vertex.z,
                vertex.nx,
                vertex.ny,
                vertex.nz,
                vertex.u,
                vertex.v,
            });
        }
        out.indices = indices;
        out.batches.reserve(character_batches.size());
        for (const auto& batch : character_batches) {
            out.batches.push_back(CharacterRenderBatch{
                batch.start_index,
                batch.index_count,
                batch.texture_path,
                batch.is_head,
            });
        }
        return out;
    }

    SkinnedCharacterModel skinned_model() const {
        SkinnedCharacterModel out;
        out.skeleton = character_skeleton;
        out.scale = character_scale;
        out.center_x = character_center_x;
        out.center_z = character_center_z;
        out.min_y = character_min_y;
        out.indices = indices;
        out.sources.reserve(character_sources.size());
        for (const auto& source : character_sources) {
            out.sources.push_back(SkinnedSource{
                source.position.x,
                source.position.y,
                source.position.z,
                source.normal.x,
                source.normal.y,
                source.normal.z,
                source.u,
                source.v,
                source.bone0,
                source.bone1,
                source.blend,
            });
        }
        out.batches.reserve(character_batches.size());
        for (const auto& batch : character_batches) {
            out.batches.push_back(SkinnedBatch{
                batch.start_index,
                batch.index_count,
                batch.texture_path,
                batch.is_head,
            });
        }
        return out;
    }

    void update_character_animation() {
        if (!vertex_buffer || character_sources.empty() || character_animation_frames == 0) {
            return;
        }

        const DWORD elapsed = GetTickCount() - character_animation_tick;
        const std::size_t frame = character_animation_start + (static_cast<std::size_t>(elapsed / 80) % character_animation_frames);
        try {
            update_character_vertices_for_frame(frame);
        } catch (...) {
            return;
        }

        const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(SceneVertex));
        if (vertex_bytes == 0) {
            return;
        }

        void* vertex_data = nullptr;
        if (SUCCEEDED(vertex_buffer->Lock(0, vertex_bytes, &vertex_data, 0))) {
            std::memcpy(vertex_data, vertices.data(), vertex_bytes);
            vertex_buffer->Unlock();
        }
    }

    void draw_overlay() {
        if (!overlay_texture || overlay_width <= 0 || overlay_height <= 0) {
            return;
        }

        const RECT rc = client_rect();
        float x = static_cast<float>(overlay_x);
        if (overlay_align_right_x) {
            x = static_cast<float>((rc.right - rc.left) - overlay_width + overlay_x);
        }
        const float y = static_cast<float>(overlay_y);
        const float w = static_cast<float>(overlay_width);
        const float h = static_cast<float>(overlay_height);
        const OverlayVertex quad[] = {
            OverlayVertex{x - 0.5f,     y - 0.5f,     0.0f, 1.0f, 0xffffffff, 0.0f, 0.0f},
            OverlayVertex{x + w - 0.5f, y - 0.5f,     0.0f, 1.0f, 0xffffffff, 1.0f, 0.0f},
            OverlayVertex{x + w - 0.5f, y + h - 0.5f, 0.0f, 1.0f, 0xffffffff, 1.0f, 1.0f},
            OverlayVertex{x - 0.5f,     y + h - 0.5f, 0.0f, 1.0f, 0xffffffff, 0.0f, 1.0f},
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

    bool initialize(HWND window, const std::filesystem::path& root, std::wstring& error) {
        hwnd = window;
        root_path = root;
        female_character = false;
        start_tick = GetTickCount();
        try {
            load_character_mesh(root_path, female_character, face_index, hair_index, hair_color_index, tattoo_index);
            load_ground_mesh(root_path);
        } catch (const std::exception& ex) {
            std::ostringstream out;
            out << "3D asset load failed: " << ex.what();
            const std::string text = out.str();
            error.assign(text.begin(), text.end());
            return false;
        }

        if (!create_device(error)) {
            return false;
        }
        if (!upload_buffers(error)) {
            return false;
        }

        configure_render_state();
        set_camera_focus(0);
        snap_camera_to_target();
        initialized = true;
        return true;
    }

    bool set_overlay_bitmap(int width, int height, int x, int y, bool align_right_x, std::vector<std::uint8_t> bgra_pixels, std::wstring& error) {
        if (!device) {
            error = L"set_overlay_bitmap called before Direct3D device creation";
            return false;
        }
        if (width <= 0 || height <= 0) {
            release_com(overlay_texture);
            overlay_width = 0;
            overlay_height = 0;
            return true;
        }
        const std::size_t expected_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        if (bgra_pixels.size() < expected_size) {
            error = L"overlay bitmap is truncated";
            return false;
        }

        HRESULT hr = S_OK;
        if (!overlay_texture || overlay_width != width || overlay_height != height) {
            release_com(overlay_texture);
            hr = device->CreateTexture(
                static_cast<UINT>(width),
                static_cast<UINT>(height),
                1,
                0,
                D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,
                &overlay_texture,
                nullptr);
            if (FAILED(hr)) {
                error = hresult_text("CreateTexture overlay", hr);
                return false;
            }
        }

        D3DLOCKED_RECT locked{};
        hr = overlay_texture->LockRect(0, &locked, nullptr, 0);
        if (FAILED(hr)) {
            release_com(overlay_texture);
            error = hresult_text("OverlayTexture::LockRect", hr);
            return false;
        }

        const auto* source = bgra_pixels.data();
        auto* dest = static_cast<std::uint8_t*>(locked.pBits);
        const std::size_t source_pitch = static_cast<std::size_t>(width) * 4;
        for (int row = 0; row < height; ++row) {
            std::memcpy(dest + static_cast<std::size_t>(row) * locked.Pitch, source + static_cast<std::size_t>(row) * source_pitch, source_pitch);
        }
        overlay_texture->UnlockRect(0);

        overlay_width = width;
        overlay_height = height;
        overlay_x = x;
        overlay_y = y;
        overlay_align_right_x = align_right_x;
        return true;
    }

    bool validate_character_appearance(bool female, int face, int hair, int hair_color, int tattoo, std::wstring& error) {
        try {
            const auto subobjects = load_xadd_subobjects(root_path / "xadd" / "subobjs.dat");
            const auto codes = character_subobject_codes(female, face, hair);
            for (const auto& code : codes) {
                const auto entry_it = subobjects.find(code);
                if (entry_it == subobjects.end()) {
                    throw std::runtime_error("missing subobject code in subobjs.dat: " + code);
                }
                const auto& entry = entry_it->second;
                const std::size_t texture_index = texture_index_for_subobject(entry, hair_color, tattoo);
                const auto chr_path = root_path / "xadd" / (entry.mesh_name + ".chr");
                if (!std::filesystem::exists(chr_path)) {
                    throw std::runtime_error("missing CHR mesh: " + chr_path.string());
                }
                const auto texture_path = model_material_texture_path(root_path, entry.texture_names[texture_index]);
                if (!std::filesystem::exists(texture_path)) {
                    throw std::runtime_error("missing character texture: " + texture_path.string());
                }
            }
        } catch (const std::exception& ex) {
            std::ostringstream out;
            out << "XADD character validation failed: " << ex.what();
            assign_error(error, out.str());
            return false;
        }
        return true;
    }

    bool set_character_appearance(bool female, int face, int hair, int hair_color, int tattoo, std::wstring& error) {
        if (female == female_character &&
            face == face_index &&
            hair == hair_index &&
            hair_color == hair_color_index &&
            tattoo == tattoo_index) {
            return true;
        }
        if (!validate_character_appearance(female, face, hair, hair_color, tattoo, error)) {
            return false;
        }

        for (auto& batch : character_batches) {
            release_com(batch.texture);
        }
        try {
            load_character_mesh(root_path, female, face, hair, hair_color, tattoo);
        } catch (const std::exception& ex) {
            std::ostringstream out;
            out << "XADD character load failed: " << ex.what();
            assign_error(error, out.str());
            return false;
        }
        female_character = female;
        face_index = face;
        hair_index = hair;
        hair_color_index = hair_color;
        tattoo_index = tattoo;

        for (auto& batch : ground_batches) {
            release_com(batch.texture);
        }
        release_com(ground_index_buffer);
        release_com(ground_vertex_buffer);
        release_com(index_buffer);
        release_com(vertex_buffer);
        if (!upload_buffers(error)) {
            return false;
        }
        configure_render_state();
        return true;
    }

    bool set_character_gender(bool female, std::wstring& error) {
        return set_character_appearance(female, face_index, hair_index, hair_color_index, tattoo_index, error);
    }

    void resize() {
        if (!device) {
            return;
        }

        fill_present_parameters();
        const HRESULT hr = device->Reset(&present);
        if (SUCCEEDED(hr)) {
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

        update_camera();
        update_view_projection();
        update_character_animation();
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(7, 8, 7), 1.0f, 0);
        if (SUCCEEDED(device->BeginScene())) {
            draw_ground();
            draw_character();
            draw_overlay();
            device->EndScene();
        }
        device->Present(nullptr, nullptr, nullptr, nullptr);
    }
};

CharacterSelectScene::CharacterSelectScene() : impl_(std::make_unique<Impl>()) {}

CharacterSelectScene::~CharacterSelectScene() = default;

bool CharacterSelectScene::initialize(HWND hwnd, const std::filesystem::path& root, std::wstring& error) {
    return impl_->initialize(hwnd, root, error);
}

void CharacterSelectScene::set_camera_profiles(const CharacterCameraProfileTable& profiles) {
    if (impl_) {
        impl_->set_camera_profiles(profiles);
    }
}

bool CharacterSelectScene::set_overlay_bitmap(int width, int height, int x, int y, bool align_right_x, std::vector<std::uint8_t> bgra_pixels, std::wstring& error) {
    return impl_->set_overlay_bitmap(width, height, x, y, align_right_x, std::move(bgra_pixels), error);
}

bool CharacterSelectScene::set_character_gender(bool female, std::wstring& error) {
    return impl_->set_character_gender(female, error);
}

bool CharacterSelectScene::set_character_appearance(bool female, int face, int hair, int hair_color, int tattoo, std::wstring& error) {
    return impl_->set_character_appearance(female, face, hair, hair_color, tattoo, error);
}

CharacterRenderMesh CharacterSelectScene::character_render_mesh() const {
    return impl_->character_render_mesh();
}

SkinnedCharacterModel CharacterSelectScene::skinned_model() const {
    return impl_->skinned_model();
}

void CharacterSelectScene::set_camera_focus(int focus_id) {
    if (impl_) {
        impl_->set_camera_focus(focus_id);
    }
}

void CharacterSelectScene::rotate_character(float radians_delta) {
    if (impl_) {
        impl_->angle += radians_delta;
    }
}

void CharacterSelectScene::resize() {
    impl_->resize();
}

void CharacterSelectScene::render() {
    impl_->render();
}

bool CharacterSelectScene::valid() const {
    return impl_ && impl_->initialized;
}

} // namespace sphere::client
