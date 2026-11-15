#ifndef ENGINE_SOURCE_RETAIL_ARMORLAYERS_H
#define ENGINE_SOURCE_RETAIL_ARMORLAYERS_H

// Layer compositing for Retail armor textures (ItemDisplayInfoMaterialRes sections), shared by the D3D11 and
// Vulkan back ends: the character skin is composed on the CPU into a BGRA mip chain and uploaded afterwards, so
// one code path serves both. Everything here is bounds-checked; nothing writes outside the section rectangle.
//
// The stock section paster only understands palettized BLPs (it paints bright green for anything else) and picks
// source mips with an unbounded `while (width != size)` loop. Retail armor ships DXT1/3/5 or raw BLPs of arbitrary
// size, so the client routes Retail-named layers through PasteLayer instead.

#include "Engine/Source/Retail/Compose.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Retail::Armor {

enum class LayerOp : uint8_t { Copy, Alpha, Multiply, Overlay, Screen };

// Compose.cpp blend codes: 0 copy, 2 straight alpha-over, 4 multiply, 6 overlay, 7 screen.
uint32_t BlendCode(LayerOp op);

// Byte length of a BLP2 file image derived from its header (end of the furthest mip), or 0 when the header is not a
// sane BLP2 or claims more than `available` bytes.
size_t BlpExtent(const uint8_t* data, size_t available);

// Decodes mip 0 of any BLP2 encoding. Rejects headers that overrun `available` and sizes above 8192 per side.
bool DecodeLayer(const uint8_t* data, size_t available, Image& out, std::string& error);

struct SectionRect { uint32_t x, y, width, height; };

// Blends `layer` over `section` of a square BGRA texture (`textureSize` pixels per side, mips[0] full size, each next
// level half the size; null or missing levels end the chain), then refreshes the covered texels of every lower mip.
// Returns false, touching nothing, when the section leaves the texture or the layer is empty/inconsistent.
bool PasteLayer(uint8_t* const* mips, uint32_t mipCount, uint32_t textureSize, const SectionRect& section,
                const Image& layer, LayerOp op);

}
#endif
