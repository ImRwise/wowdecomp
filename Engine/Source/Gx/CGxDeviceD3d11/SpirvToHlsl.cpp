#include "Engine/Source/Gx/CGxDeviceD3d11/SpirvToHlsl.h"

#include <exception>
#include <vector>

#include "ThirdParty/spirv-cross/spirv_hlsl.hpp"

namespace D3d11SpirvToHlsl {

namespace {
    constexpr uint32_t kConstantsBinding = 16;
    constexpr uint32_t kFirstImageBinding = 17;
    // b0..b3 are the engine's own constant buffers: the effects' go above them
    constexpr uint32_t kConstantsRegister = 4;
    constexpr uint32_t kPushRegister = 5;
    constexpr uint32_t kOtherBlockRegister = 6;
}

Result Convert(const uint32_t* words, size_t wordCount) {
    Result result;
    try {
        spirv_cross::CompilerHLSL compiler(words, wordCount);

        const auto entries = compiler.get_entry_points_and_stages();
        if (entries.empty()) {
            result.error = "no entry point";
            return result;
        }
        const spv::ExecutionModel model = entries[0].execution_model;
        const char* profile = "";
        if (model == spv::ExecutionModelVertex) {
            result.stage = Stage::Vertex;
        } else if (model == spv::ExecutionModelGLCompute) {
            result.stage = Stage::Compute;
        } else if (model == spv::ExecutionModelFragment) {
            result.stage = Stage::Fragment;
        } else {
            result.error = "unsupported shader stage";
            return result;
        }
        (void)profile;

        spirv_cross::CompilerHLSL::Options hlsl;
        hlsl.shader_model = 50;
        hlsl.point_size_compat = true;    // D3D11 has no point size: points are one pixel
        hlsl.point_coord_compat = true;
        compiler.set_hlsl_options(hlsl);

        spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
        common.vertex.flip_vert_y = result.stage == Stage::Vertex;
        common.vertex.fixup_clipspace = false;
        compiler.set_common_options(common);

        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        auto bindingFor = [&](uint32_t set, uint32_t binding) {
            spirv_cross::HLSLResourceBinding b = {};
            b.stage = model;
            b.desc_set = set;
            b.binding = binding;
            return b;
        };

        for (const auto& image : resources.sampled_images) {
            const uint32_t binding = compiler.get_decoration(image.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b = bindingFor(compiler.get_decoration(image.id, spv::DecorationDescriptorSet), binding);
            b.srv.register_space = 0;
            b.srv.register_binding = binding;
            b.sampler.register_space = 0;
            b.sampler.register_binding = binding;
            compiler.add_hlsl_resource_binding(b);
            if (compiler.get_type(image.type_id).image.dim == spv::Dim3D && binding < 32) {
                result.volumeMask |= 1u << binding;
            }
        }
        for (const auto& image : resources.separate_images) {
            const uint32_t binding = compiler.get_decoration(image.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b = bindingFor(compiler.get_decoration(image.id, spv::DecorationDescriptorSet), binding);
            b.srv.register_binding = binding;
            compiler.add_hlsl_resource_binding(b);
        }
        for (const auto& sampler : resources.separate_samplers) {
            const uint32_t binding = compiler.get_decoration(sampler.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b = bindingFor(compiler.get_decoration(sampler.id, spv::DecorationDescriptorSet), binding);
            b.sampler.register_binding = binding;
            compiler.add_hlsl_resource_binding(b);
        }
        for (const auto& image : resources.storage_images) {
            const uint32_t binding = compiler.get_decoration(image.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b = bindingFor(compiler.get_decoration(image.id, spv::DecorationDescriptorSet), binding);
            b.uav.register_space = 0;
            b.uav.register_binding = binding >= kFirstImageBinding ? binding - kFirstImageBinding : binding;
            compiler.add_hlsl_resource_binding(b);
        }
        for (const auto& block : resources.uniform_buffers) {
            const uint32_t binding = compiler.get_decoration(block.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b = bindingFor(compiler.get_decoration(block.id, spv::DecorationDescriptorSet), binding);
            b.cbv.register_space = 0;
            b.cbv.register_binding = binding == kConstantsBinding ? kConstantsRegister : kOtherBlockRegister + binding;
            compiler.add_hlsl_resource_binding(b);
        }
        if (!resources.push_constant_buffers.empty()) {
            spirv_cross::HLSLResourceBinding b = {};
            b.stage = model;
            b.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
            b.binding = spirv_cross::ResourceBindingPushConstantBinding;
            b.cbv.register_space = 0;
            b.cbv.register_binding = kPushRegister;
            compiler.add_hlsl_resource_binding(b);
            result.usesPush = true;
            result.pushBytes = static_cast<uint32_t>(compiler.get_declared_struct_size(compiler.get_type(resources.push_constant_buffers[0].base_type_id)));
        }

        if (result.stage == Stage::Compute) {
            const spirv_cross::SPIREntryPoint& entry = compiler.get_entry_point(entries[0].name, model);
            result.groupSize[0] = entry.workgroup_size.x;
            result.groupSize[1] = entry.workgroup_size.y;
            result.groupSize[2] = entry.workgroup_size.z;
        }

        result.hlsl = compiler.compile();
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    } catch (...) {
        result.error = "unknown error";
    }
    return result;
}

}  // namespace D3d11SpirvToHlsl
