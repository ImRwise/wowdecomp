#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXD3D11EFFECTS_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXD3D11EFFECTS_H

#include <cstdint>
#include <d3d11.h>

#include "Engine/Source/Gx/GxEffects.h"

// Not in the original binary. GxEffects over Direct3D 11 (CGxD3d11Effects.cpp): full-screen and point passes, compute
// passes over unordered-access textures, copies of the world's colour and depth. The programs are the SPIR-V modules of
// the Vulkan backend, translated with SPIRV-Cross (SpirvToHlsl.h).

// For the engine's own draws (CGxD3d11State.cpp's IBindTextures): the view and sampler of what GxEffects::SetDrawTexture
// put on `stage`, or false when nothing did.
bool D3d11EffectsDrawTexture(uint32_t stage, ID3D11ShaderResourceView*& view, ID3D11SamplerState*& sampler);

#endif
