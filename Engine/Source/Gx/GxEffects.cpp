// Not in the original binary. The state of the GxEffects interface that is the same on every backend: the engine-draw
// texture overrides and the forced depth write; and which backend's device GxEffects::Get() hands out.
#include "Engine/Source/Gx/GxEffects.h"

namespace {
    bool g_forceDepthWrite = false;
    GxEffects::DrawOverride g_drawOverride[GxEffects::kMaxTextures];
}

void GxEffects::ForceDepthWrite(bool on) {
    g_forceDepthWrite = on;
}

bool GxEffects::DepthWriteForced() {
    return g_forceDepthWrite;
}

void GxEffects::SetDrawTexture(uint32_t stage, Texture* texture, bool linear, bool wrap, bool mips) {
    if (stage >= kMaxTextures) {
        return;
    }

    g_drawOverride[stage].texture = texture;
    g_drawOverride[stage].linear = linear;
    g_drawOverride[stage].wrap = wrap;
    g_drawOverride[stage].mips = mips;
}

const GxEffects::DrawOverride& GxEffects::DrawOverrideFor(uint32_t stage) {
    static const DrawOverride none;
    return stage < kMaxTextures ? g_drawOverride[stage] : none;
}

#ifndef _WIN32
// only Windows has the Vulkan and D3D11 backends
GxEffects::Device* GxEffects::VulkanDevice() {
    return nullptr;
}

GxEffects::Device* GxEffects::D3d11Device() {
    return nullptr;
}
#endif

GxEffects::Device* GxEffects::Get() {
    if (Device* device = VulkanDevice()) {
        return device;
    }

    return D3d11Device();
}
