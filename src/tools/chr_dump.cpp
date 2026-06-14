#include "model/chr.hpp"

#include <iomanip>
#include <iostream>

namespace {

void print_hex_offset(std::size_t value) {
    std::cout << "0x" << std::hex << std::uppercase << value << std::dec << std::nouppercase;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: chr_dump <file.chr>\n";
        return 2;
    }

    try {
        const auto info = sphere::model::load_chr_info(argv[1]);
        std::cout << "file: " << info.path.string() << "\n";
        std::cout << "size: " << info.file_size << "\n";
        std::cout << "version_or_flags: " << info.version_or_flags << "\n";
        std::cout << "chunks: " << info.chunk_count << "\n";

        std::cout << "\nchunk_table:\n";
        for (std::size_t i = 0; i < info.chunks.size(); ++i) {
            const auto& chunk = info.chunks[i];
            std::cout << "  [" << i << "] type=" << chunk.type << " offset=";
            print_hex_offset(chunk.offset);
            std::cout << " size=" << chunk.size;
            if (chunk.type == 1 && chunk.size % 0x28 == 0) {
                std::cout << " records_0x28=" << (chunk.size / 0x28);
            }
            std::cout << "\n";
        }

        if (!info.bone_names.empty()) {
            std::cout << "\nbone_names:\n";
            for (std::size_t i = 0; i < info.bone_names.size(); ++i) {
                std::cout << "  [" << i << "] " << info.bone_names[i] << "\n";
            }
        }

        const auto mesh = sphere::model::load_chr_mesh(argv[1]);
        if (!mesh.vertices.empty() || !mesh.indices.empty()) {
            std::cout << "\nmesh:\n";
            std::cout << "  vertices: " << mesh.vertices.size() << "\n";
            std::cout << "  indices: " << mesh.indices.size() << "\n";
            std::cout << "  triangles: " << (mesh.indices.size() / 3) << "\n";
            std::cout << "  bounds: ["
                      << mesh.bounds.min_x << ", " << mesh.bounds.min_y << ", " << mesh.bounds.min_z
                      << "] - ["
                      << mesh.bounds.max_x << ", " << mesh.bounds.max_y << ", " << mesh.bounds.max_z
                      << "]\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "chr_dump: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
