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

Ray-marches the terrain's layer textures against a smooth height field (their luminance at a blurred mip, so grain such as
sand does not become spikes, blended with the layers' own alpha weights). The relief is weighted by how structured the
surface is: stones, bricks and cracks get depth, sand and grass stay flat. The height field's slope also gives a normal for
the sun's shading and a faint glint, and a gentle self-shadow toward the sun that never darkens the sun light below 76%. The tangent frame comes from screen derivatives, so no
ADT change is needed. It runs in our terrain shaders: with `visuals 2` as `hdTerrainLight` says (1 Forever lighting, 2 and 3 as
described in `HdTerrain.h`, 0 Blizzard's shaders and so no parallax), and with `visuals 1` (Lumenveil, the default) whenever
`terrainPOM` is on, then with 3.3.5's own lighting. `visuals 0` (stock) draws nothing extra.

| CVar | Default | Meaning |
| --- | --- | --- |
| `terrainPOM` | `1` | 0 off, 1 on |
| `terrainPOMDepth` | `0.018` | depth in 8-yard texture tiles (0.018 is about 0.14 yards) |
| `terrainPOMSamples` | `16` | ray steps at most, 4..16 |
| `terrainPOMFadeStart` | `25` | yards; the effect starts to fade here |
| `terrainPOMDistance` | `55` | yards; the effect is gone here, nothing is done beyond |
| `terrainPOMShadow` | `0.24` | 0..0.3, how much the relief's self-shadow darkens the sun light (0.24 = never below 76%) |

Example: `/console terrainPOMDepth 0.04` for deeper cobbles, `/console terrainPOMSamples 8` to save fill rate.

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

`hdBloomThreshold` (0..1, 0.82, measured on luma so saturated ground does not glow) sets how bright a pixel must be to glow; `hdBloomDebug` 1 mask, 2 rays, 3 halo.

## D3D11 effects

The D3D11 backend (`-d3d11`) implements `GxEffects` too, so bloom, sun shafts, fog, water, AO, GI and Lumenveil's passes can
run on it. The effect programs are the SPIR-V modules the Vulkan backend uses; D3D11 turns each into Shader Model 5 HLSL with
SPIRV-Cross (`ThirdParty/spirv-cross`, Apache-2.0) when the effect first creates it, and keeps the compiled result in
`Cache\D3d11Effects` beside the exe (delete the folder to rebuild it). The first start with the effects on spends a few seconds
compiling the compute shaders. Terrain parallax needs nothing: D3D11 compiles `HdTerrain`'s HLSL itself.

What it needs from the device: the default depth buffer is typeless and sampleable (a multisampled one is read through sample 0),
effect textures can be unordered-access targets, and `GxEffects::SetDrawTexture` / `ForceDepthWrite` reach the engine's own draws.

## Status

- Done: terrain POM (luminance height), aliases above, D3D11 `GxEffects` (builds; **not yet run in game**: the first test on D3D11
  may show problems in the passes, `Logs\d3d11.log` lines starting `GxEffects:` say what failed).
- Offline check: all 71 effect programs convert to HLSL and compile for SM5.
- Not done yet: MSAA in the Vulkan backend (it renders at one sample; the effects sample the scene mid-frame, so a resolve
  is needed at each of those points), `_h.blp` height textures and a per-pixel specular (the retail ADT adapter does not read MHID,
  and the chunk renderer binds no extra terrain sampler), automatic translation of `VulkanShaders` in the CMake build.
