#include "Engine/Source/Retail/ArmorLayers.h"

#include <algorithm>
#include <cstring>

namespace Retail::Armor {

uint32_t BlendCode(LayerOp op) {
    switch (op) {
    case LayerOp::Copy: return 0;
    case LayerOp::Alpha: return 2;
    case LayerOp::Multiply: return 4;
    case LayerOp::Overlay: return 6;
    case LayerOp::Screen: return 7;
    }
    return 2;
}

namespace {
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
constexpr size_t kBlpHeader = 148;  // magic .. mip size table
constexpr uint32_t kMaxSide = 8192;
}

size_t BlpExtent(const uint8_t* data, size_t available) {
    if (!data || available < kBlpHeader || std::memcmp(data, "BLP2", 4) != 0 || U32(data + 4) != 1) return 0;
    const uint32_t width = U32(data + 12), height = U32(data + 16);
    if (!width || !height || width > kMaxSide || height > kMaxSide) return 0;
    size_t extent = kBlpHeader;
    for (uint32_t level = 0; level < 16; ++level) {
        const uint32_t offset = U32(data + 20 + level * 4), size = U32(data + 84 + level * 4);
        if (!size) continue;
        if (size_t(offset) + size > available) return 0;  // a mip that runs past the file: do not trust any of it
        extent = std::max(extent, size_t(offset) + size);
    }
    return extent;
}

bool DecodeLayer(const uint8_t* data, size_t available, Image& out, std::string& error) {
    out = {};
    const size_t extent = BlpExtent(data, available);
    if (!extent) { error = "Invalid or truncated BLP2 header"; return false; }
    return DecodeBlp(std::vector<uint8_t>(data, data + extent), out, error);
}

bool PasteLayer(uint8_t* const* mips, uint32_t mipCount, uint32_t textureSize, const SectionRect& s, const Image& layer,
                LayerOp op) {
    if (!mips || !mips[0] || !mipCount || !textureSize || !s.width || !s.height) return false;
    if (uint64_t(s.x) + s.width > textureSize || uint64_t(s.y) + s.height > textureSize) return false;
    if (!layer.width || !layer.height || layer.rgba.size() != size_t(layer.width) * layer.height * 4) return false;

    // Work on a straight-RGBA copy of the section so every blend mode sees what is already painted there.
    Image rect;
    rect.width = s.width;
    rect.height = s.height;
    rect.rgba.resize(size_t(s.width) * s.height * 4);
    for (uint32_t y = 0; y < s.height; ++y) {
        const uint8_t* row = mips[0] + (size_t(s.y + y) * textureSize + s.x) * 4;
        uint8_t* out = &rect.rgba[size_t(y) * s.width * 4];
        for (uint32_t x = 0; x < s.width; ++x) {
            out[x * 4 + 0] = row[x * 4 + 2];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + 0];
            out[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    PaintImage(rect, layer, 0, 0, s.width, s.height, BlendCode(op));
    for (uint32_t y = 0; y < s.height; ++y) {
        uint8_t* row = mips[0] + (size_t(s.y + y) * textureSize + s.x) * 4;
        const uint8_t* in = &rect.rgba[size_t(y) * s.width * 4];
        for (uint32_t x = 0; x < s.width; ++x) {
            row[x * 4 + 0] = in[x * 4 + 2];
            row[x * 4 + 1] = in[x * 4 + 1];
            row[x * 4 + 2] = in[x * 4 + 0];
            row[x * 4 + 3] = 0xFF;  // the character skin sheet is opaque
        }
    }

    // Box-filter the changed texels into each lower level.
    for (uint32_t level = 1; level < mipCount && mips[level]; ++level) {
        const uint32_t size = std::max(1u, textureSize >> level), previous = std::max(1u, textureSize >> (level - 1));
        const uint32_t x0 = s.x >> level, y0 = s.y >> level;
        const uint32_t x1 = std::min(size, (s.x + s.width + (1u << level) - 1) >> level);
        const uint32_t y1 = std::min(size, (s.y + s.height + (1u << level) - 1) >> level);
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                const uint32_t sx0 = std::min(x * 2, previous - 1), sx1 = std::min(x * 2 + 1, previous - 1);
                const uint32_t sy0 = std::min(y * 2, previous - 1), sy1 = std::min(y * 2 + 1, previous - 1);
                const uint8_t* a = mips[level - 1] + (size_t(sy0) * previous + sx0) * 4;
                const uint8_t* b = mips[level - 1] + (size_t(sy0) * previous + sx1) * 4;
                const uint8_t* c = mips[level - 1] + (size_t(sy1) * previous + sx0) * 4;
                const uint8_t* d = mips[level - 1] + (size_t(sy1) * previous + sx1) * 4;
                uint8_t* dst = mips[level] + (size_t(y) * size + x) * 4;
                for (int channel = 0; channel < 4; ++channel)
                    dst[channel] = uint8_t((a[channel] + b[channel] + c[channel] + d[channel] + 2) / 4);
            }
    }
    return true;
}

}
