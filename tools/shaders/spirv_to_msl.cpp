// Converts a SPIR-V module to Metal Shading Language with SPIRV-Cross, laid out the way plume's Metal backend binds
// resources: descriptor set N is an argument buffer at buffer(N), push constants are buffer(8) and vertex attributes
// are [[attribute(location)]].
//
// Usage: spirv_to_msl <input.spv> <output.metal>

#include <spirv_msl.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kPushConstantBuffer = 8;

std::vector<uint32_t> read_spirv(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("cannot open ") + path);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0) throw std::runtime_error("not a SPIR-V module");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(words.data()));
    return words;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <input.spv> <output.metal>\n";
        return 2;
    }
    try {
        spirv_cross::CompilerMSL msl(read_spirv(argv[1]));

        spirv_cross::CompilerMSL::Options options;
        options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 1);
        options.argument_buffers = true;
        options.texture_buffer_native = true;
        options.enable_decoration_binding = true;
        msl.set_msl_options(options);

        for (const spv::ExecutionModel stage : {spv::ExecutionModelVertex, spv::ExecutionModelFragment}) {
            spirv_cross::MSLResourceBinding binding;
            binding.stage = stage;
            binding.desc_set = spirv_cross::kPushConstDescSet;
            binding.binding = spirv_cross::kPushConstBinding;
            binding.msl_buffer = kPushConstantBuffer;
            msl.add_msl_resource_binding(binding);
        }

        // Vulkan clip space is y-down and Metal's is y-up.
        spirv_cross::CompilerGLSL::Options common;
        common.vertex.flip_vert_y = true;
        msl.set_common_options(common);

        const std::string source = msl.compile();
        std::ofstream out(argv[2], std::ios::binary);
        if (!out) throw std::runtime_error(std::string("cannot write ") + argv[2]);
        out << source;
        return out ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "spirv_to_msl: " << e.what() << "\n";
        return 1;
    }
}
