#include "model/mdl.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace {

void print_hex_offset(std::size_t value) {
    std::cout << "0x" << std::hex << std::uppercase << value << std::dec << std::nouppercase;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mdl_dump <file.mdl>\n";
        return 2;
    }

    try {
        const auto mesh = sphere::model::load_mdl_mesh(argv[1]);
        const auto& info = mesh.info;
        std::cout << "file: " << info.path.string() << "\n";
        std::cout << "size: " << info.file_size << "\n";
        std::cout << "computed_size: " << info.computed_size << "\n";
        std::cout << "vertices: " << info.vertex_count << "\n";
        std::cout << "indices: " << info.index_count << "\n";
        std::cout << "triangle_groups: " << info.triangle_group_count << "\n";
        std::cout << "materials: " << static_cast<int>(info.material_count) << "\n";
        std::cout << "strip_groups: " << static_cast<int>(info.strip_group_count) << "\n";
        std::cout << "skin_weights: " << static_cast<int>(info.skin_weight_count) << "\n";
        std::cout << "animation_flag: " << static_cast<int>(info.animation_flag) << "\n";
        std::cout << "extra_mode: " << info.extra_mode << "\n";
        std::cout << "render_vertices: " << mesh.vertices.size() << "\n";
        std::cout << "render_triangles: " << mesh.triangles.size() << "\n";
        std::cout << "bounds: min=(" << mesh.bounds.min_x << ", " << mesh.bounds.min_y << ", " << mesh.bounds.min_z
                  << ") max=(" << mesh.bounds.max_x << ", " << mesh.bounds.max_y << ", " << mesh.bounds.max_z << ")\n";

        std::cout << "\nmaterial_names:\n";
        for (std::size_t i = 0; i < info.materials.size(); ++i) {
            std::cout << "  [" << i << "] " << info.materials[i] << "\n";
        }

        std::cout << "\nsections:\n";
        for (const auto& section : info.sections) {
            std::cout << "  " << std::left << std::setw(24) << section.name << std::right
                      << " offset=";
            print_hex_offset(section.offset);
            std::cout << " count=" << section.count
                      << " stride=" << section.stride
                      << " size=" << section.size << "\n";
        }

        if (!mesh.surfaces.empty()) {
            std::cout << "\nsurfaces:\n";
            for (std::size_t i = 0; i < mesh.surfaces.size(); ++i) {
                const auto& surface = mesh.surfaces[i];
                const std::size_t first_vertex = static_cast<std::size_t>((std::max)(surface.first_vertex_index, static_cast<std::int16_t>(0)));
                const std::size_t vertex_count = static_cast<std::size_t>((std::max)(surface.vertex_count, static_cast<std::int16_t>(0)));
                if (first_vertex >= mesh.vertices.size() || vertex_count == 0 || vertex_count > mesh.vertices.size() - first_vertex) {
                    std::cout << "  [" << i << "] invalid vertex range\n";
                    continue;
                }
                float min_x = mesh.vertices[first_vertex].x;
                float min_y = mesh.vertices[first_vertex].y;
                float min_z = mesh.vertices[first_vertex].z;
                float max_x = min_x;
                float max_y = min_y;
                float max_z = min_z;
                for (std::size_t v = 0; v < vertex_count; ++v) {
                    const auto& vertex = mesh.vertices[first_vertex + v];
                    min_x = (std::min)(min_x, vertex.x);
                    min_y = (std::min)(min_y, vertex.y);
                    min_z = (std::min)(min_z, vertex.z);
                    max_x = (std::max)(max_x, vertex.x);
                    max_y = (std::max)(max_y, vertex.y);
                    max_z = (std::max)(max_z, vertex.z);
                }
                std::cout << "  [" << i << "] material=" << static_cast<int>(surface.texture_index);
                if (surface.texture_index < info.materials.size()) {
                    std::cout << "(" << info.materials[surface.texture_index] << ")";
                }
                std::cout << " object=" << static_cast<int>(surface.object_index)
                          << " tris=" << surface.triangle_count
                          << " verts=" << surface.vertex_count
                          << " bounds min=(" << min_x << ", " << min_y << ", " << min_z
                          << ") max=(" << max_x << ", " << max_y << ", " << max_z << ")\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "mdl_dump: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
