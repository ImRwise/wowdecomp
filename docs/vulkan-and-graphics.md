# Vulkan backend and HD graphics: switches and tuning

All of these are saved video CVars (Config.wtf). Set them live with `/console <name> <value>` or in the options pages.
The HD effects need visuals 2 (Azeroth Remastered), and the post effects (bloom, sun shafts, fog, water) need a backend
that provides `GxEffects`: **only Vulkan does today** (see [Status](#status)).

## Choosing the backend

| How | Value |
| --- | --- |
| Command line | `-vulkan`, `-d3d11`, `-d3d9ex` (the command line wins over the CVar) |
| CVar | `/console gxApi Vulkan` (also `D3D11`, `D3D9Ex`, `OpenGL`), then restart |

Vulkan only draws the world with the real shaders when `VulkanShaders\manifest.txt` sits beside the exe (made by
`Tools/VulkanShaders/translate_shaders.py`, see that README); without it the world falls back to fixed-function.

## Terrain parallax occlusion mapping (`HdTerrain`)

Ray-marches the terrain's layer textures against a height field made of their luminance, blended with the layers'
own alpha weights, with a soft self-shadow toward the sun. The tangent frame comes from screen derivatives, so no
ADT change is needed. It runs in our terrain shaders: with `visuals 2` as `hdTerrainLight` says (1 Forever lighting, 2 and 3 as
described in `HdTerrain.h`, 0 Blizzard's shaders and so no parallax), and with `visuals 1` (Lumenveil, the default) whenever
`terrainPOM` is on, then with 3.3.5's own lighting. `visuals 0` (stock) draws nothing extra.

| CVar | Default | Meaning |
| --- | --- | --- |
| `terrainPOM` | `1` | 0 off, 1 on |
| `terrainPOMDepth` | `0.045` | depth in 8-yard texture tiles (0.045 is about 0.36 yards) |
| `terrainPOMSamples` | `16` | ray steps at most, 4..16 |
| `terrainPOMDistance` | `85` | yards; the effect fades over the last third, nothing is done beyond |
| `terrainPOMShadow` | `0.6` | 0..1 strength of the relief's self-shadow from the sun |

Example: `/console terrainPOMDepth 0.08` for deeper cobbles, `/console terrainPOMSamples 8` to save fill rate.

**Vulkan:** the terrain pixel shaders need translated modules. After changing `HdTerrain.cpp`'s HLSL, start the game once (it
writes the missing code to `VulkanShaders\hd-pending`), then run `python Tools/HdMaterials/install_hd_shaders.py --client "<client>"`
and restart. The tool only adds or replaces its own records in `manifest.txt` (the stock and other HD modules stay) and needs glslang
at `build/tools/glslang/bin/glslang.exe` (the Khronos `main-tot` Windows release). Until then Vulkan keeps Blizzard's terrain shaders.
D3D11 compiles them directly.

## Sun shafts and bloom

`sunShafts` / `hdrBloom` are other names for the existing `hdRays` / `hdBloom`; use either. An effect is on only when
both of its names are on, and the strengths multiply, with each name's own default counting as 1.

| CVar | Default | Same as |
| --- | --- | --- |
| `sunShafts` | `1` | `hdRays` |
| `sunShaftsIntensity` | `0.65` | `hdRaysStrength` (default 1) |
| `hdrBloom` | `1` | `hdBloom` |
| `hdrBloomIntensity` | `0.45` | `hdBloomStrength` (default 1) |

`hdBloomThreshold` (0..1, 0.7) sets how bright a pixel must be to glow; `hdBloomDebug` 1 mask, 2 rays, 3 halo.

## Status

- Done: terrain POM (luminance height), aliases above.
- Not done yet: MSAA in the Vulkan backend (it renders at one sample; the effects sample the scene mid-frame, so a resolve
  is needed at each of those points), `GxEffects` on D3D11 (bloom, shafts, fog and water are Vulkan-only), `_h.blp` height
  textures and a per-pixel specular (the retail ADT adapter does not read MHID, and the chunk renderer binds no extra terrain
  sampler), automatic translation of `VulkanShaders` in the CMake build.
