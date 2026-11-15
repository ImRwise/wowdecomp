#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_SPIRVTOHLSL_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_SPIRVTOHLSL_H

#include <cstddef>
#include <cstdint>
#include <string>

// Not in the original binary. GxEffects programs are SPIR-V modules (the Vulkan backend's native form); the D3D11 backend
// turns each one into Shader Model 5 HLSL with SPIRV-Cross and compiles that. The GxEffects interface is fixed by
// binding, and the HLSL comes out with the registers the D3D11 device binds:
//
//   combined sampler2D / sampler3D at binding N   t<N> and s<N>
//   Constants uniform block (binding 16)           cbuffer b4 (b0..b3 are the engine's own)
//   push constants                                 cbuffer b5
//   storage image at binding 17 + N                u<N>
//
// A vertex module's clip-space y is flipped back: the modules were written for Vulkan's y-down clip space and D3D11's is
// y-up, like D3D9's.
namespace D3d11SpirvToHlsl {

enum class Stage : uint32_t { Vertex, Fragment, Compute };

struct Result {
    bool ok = false;
    Stage stage = Stage::Fragment;
    std::string hlsl;
    std::string error;
    uint32_t groupSize[3] = { 1, 1, 1 };   // compute: the local size
    bool usesPush = false;
    uint32_t pushBytes = 0;
    uint32_t volumeMask = 0;               // bit N: the sampler at binding N is a sampler3D
};

Result Convert(const uint32_t* words, size_t wordCount);

}  // namespace D3d11SpirvToHlsl

#endif
