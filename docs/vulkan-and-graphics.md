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

## Terrain and shafts: removed

Terrain parallax occlusion mapping and the HdRays / `sunShafts` radial pass were tried and taken out again (they distorted the
terrain textures and smeared foliage colour across the screen). `HdTerrain` is back to its lighting-only shaders (`hdTerrainLight`),
and `terrainPOM*`, `hdRays*` and `sunShafts*` no longer exist. Lumenveil's own shafts are separate: `lvShafts 0` turns them off.

## Bloom and grade

`hdrBloom` is another name for `hdBloom`; use either (both must be on, and the strengths multiply, each name's default counting as 1).

| CVar | Default | Meaning |
| --- | --- | --- |
| `hdBloom` / `hdrBloom` | `1` | on/off |
| `hdBloomStrength` | `0.5` | halo strength 0..5 |
| `hdrBloomIntensity` | `0.45` | multiplies `hdBloomStrength` (0.45 counts as 1) |
| `hdBloomThreshold` | `0.92` | 0..1.5, on luma: only near-white pixels glow, saturated ground never does (the picture tops out at 1, so above it only the soft knee blooms) |
| `hdGradeStrength` | `0.5` | colour grading strength 0..2 |

Only with `visuals 2`.

## D3D11 effects

The D3D11 backend (`-d3d11`) implements `GxEffects` too, so bloom, sun shafts, fog, water, AO, GI and Lumenveil's passes can
run on it. The effect programs are the SPIR-V modules the Vulkan backend uses; D3D11 turns each into Shader Model 5 HLSL with
SPIRV-Cross (`ThirdParty/spirv-cross`, Apache-2.0) when the effect first creates it, and keeps the compiled result in
`Cache\D3d11Effects` beside the exe (delete the folder to rebuild it). The first start with the effects on spends a few seconds
compiling the compute shaders. Terrain parallax needs nothing: D3D11 compiles `HdTerrain`'s HLSL itself.

What it needs from the device: the default depth buffer is typeless and sampleable (a multisampled one is read through sample 0),
effect textures can be unordered-access targets, and `GxEffects::SetDrawTexture` / `ForceDepthWrite` reach the engine's own draws.

## Status

- Done: bloom tuning, D3D11 `GxEffects` (builds; **not yet run in game**: the first test on D3D11
  may show problems in the passes, `Logs\d3d11.log` lines starting `GxEffects:` say what failed).
- Offline check: all 71 effect programs convert to HLSL and compile for SM5.
- Not done yet: MSAA in the Vulkan backend (it renders at one sample; the effects sample the scene mid-frame, so a resolve
  is needed at each of those points), `_h.blp` height textures and a per-pixel specular (the retail ADT adapter does not read MHID,
  and the chunk renderer binds no extra terrain sampler), automatic translation of `VulkanShaders` in the CMake build.
